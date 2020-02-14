// Copyright 2019 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "google/cloud/storage/parallel_upload.h"

namespace google {
namespace cloud {
namespace storage {
inline namespace STORAGE_CLIENT_NS {
namespace internal {

class ParallelObjectWriteStreambuf : public ObjectWriteStreambuf {
 public:
  ParallelObjectWriteStreambuf(
      std::shared_ptr<ParallelUploadStateImpl> state, std::size_t stream_idx,
      std::unique_ptr<ResumableUploadSession> upload_session,
      std::size_t max_buffer_size,
      std::unique_ptr<HashValidator> hash_validator)
      : ObjectWriteStreambuf(std::move(upload_session), max_buffer_size,
                             std::move(hash_validator)),
        state_(std::move(state)),
        stream_idx_(stream_idx) {}

  ~ParallelObjectWriteStreambuf() override {
    state_->StreamDestroyed(stream_idx_);
  }

  StatusOr<ResumableUploadResponse> Close() override {
    auto res = this->ObjectWriteStreambuf::Close();
    state_->StreamFinished(stream_idx_, res);
    return res;
  }

 private:
  std::shared_ptr<ParallelUploadStateImpl> state_;
  std::size_t stream_idx_;
};

ParallelUploadStateImpl::ParallelUploadStateImpl(
    bool cleanup_on_failures, std::string destination_object_name,
    std::int64_t expected_generation, std::shared_ptr<ScopedDeleter> deleter,
    Composer composer)
    : deleter_(std::move(deleter)),
      composer_(std::move(composer)),
      destination_object_name_(std::move(destination_object_name)),
      expected_generation_(expected_generation),
      finished_{},
      num_unfinished_streams_{} {
  if (!cleanup_on_failures) {
    deleter_->Enable(false);
  }
}

ParallelUploadStateImpl::~ParallelUploadStateImpl() {
  WaitForCompletion().wait();
}

StatusOr<ObjectWriteStream> ParallelUploadStateImpl::CreateStream(
    RawClient& raw_client, ResumableUploadRequest const& request) {
  auto session = raw_client.CreateResumableSession(request);
  std::unique_lock<std::mutex> lk(mu_);
  if (!session) {
    // Preserve the first error.
    res_ = session.status();
    return std::move(session).status();
  }

  streams_.emplace_back(
      StreamInfo{request.object_name(), (*session)->session_id(), {}, false});
  auto idx = num_unfinished_streams_++;
  lk.unlock();
  return ObjectWriteStream(
      google::cloud::internal::make_unique<ParallelObjectWriteStreambuf>(
          shared_from_this(), idx, *std::move(session),
          raw_client.client_options().upload_buffer_size(),
          CreateHashValidator(request)));
}

std::string ParallelUploadPersistentState::ToString() const {
  auto json_streams = internal::nl::json::array();
  for (auto const& stream : streams) {
    json_streams.emplace_back(internal::nl::json{
        {"name", stream.object_name},
        {"resumable_session_id", stream.resumable_session_id}});
  }
  return internal::nl::json{{"streams", json_streams},
                            {"expected_generation", expected_generation},
                            {"destination", destination_object_name}}
      .dump();
}

StatusOr<ParallelUploadPersistentState>
ParallelUploadPersistentState::FromString(std::string const& json_rep) {
  ParallelUploadPersistentState res;

  auto json = internal::nl::json::parse(json_rep, nullptr, false);
  if (json.is_discarded()) {
    return Status(StatusCode::kInternal,
                  "Parallel upload state is not a valid JSON.");
  }
  if (!json.is_object()) {
    return Status(StatusCode::kInternal,
                  "Parallel upload state is not a JSON object.");
  }
  // nl::json doesn't allow for multiple keys with the same name, so there are
  // either 0 or 1 elements with the same key.
  if (json.count("destination") != 1) {
    return Status(StatusCode::kInternal,
                  "Parallel upload state doesn't contain a 'destination'.");
  }
  auto& destination_json = json["destination"];
  if (!destination_json.is_string()) {
    return Status(StatusCode::kInternal,
                  "Parallel upload state's 'destination' is not a string.");
  }
  res.destination_object_name = destination_json;
  if (json.count("expected_generation") != 1) {
    return Status(
        StatusCode::kInternal,
        "Parallel upload state doesn't contain a 'expected_generation'.");
  }
  auto& expected_generation_json = json["expected_generation"];
  if (!expected_generation_json.is_number()) {
    return Status(
        StatusCode::kInternal,
        "Parallel upload state's 'expected_generation' is not a number.");
  }
  res.expected_generation = expected_generation_json;
  if (json.count("streams") != 1) {
    return Status(StatusCode::kInternal,
                  "Parallel upload state doesn't contain 'streams'.");
  }
  auto& streams_json = json["streams"];
  if (!streams_json.is_array()) {
    return Status(StatusCode::kInternal,
                  "Parallel upload state's 'streams' is not an array.");
  }
  for (auto& stream_json : streams_json) {
    if (!stream_json.is_object()) {
      return Status(StatusCode::kInternal,
                    "Parallel upload state's 'stream' is not an object.");
    }
    if (stream_json.count("name") != 1) {
      return Status(StatusCode::kInternal,
                    "Parallel upload state's stream doesn't contain a 'name'.");
    }
    auto object_name_json = stream_json["name"];
    if (!object_name_json.is_string()) {
      return Status(StatusCode::kInternal,
                    "Parallel upload state's stream 'name' is not a string.");
    }
    if (stream_json.count("resumable_session_id") != 1) {
      return Status(StatusCode::kInternal,
                    "Parallel upload state's stream doesn't contain a "
                    "'resumable_session_id'.");
    }
    auto resumable_session_id_json = stream_json["resumable_session_id"];
    if (!resumable_session_id_json.is_string()) {
      return Status(StatusCode::kInternal,
                    "Parallel upload state's stream 'resumable_session_id' is "
                    "not a string.");
    }
    res.streams.emplace_back(ParallelUploadPersistentState::Stream{
        object_name_json, resumable_session_id_json});
  }
  if (res.streams.empty()) {
    return Status(StatusCode::kInternal,
                  "Parallel upload state's stream doesn't contain any streams");
  }
  return res;
}

Status ParallelUploadStateImpl::EagerCleanup() {
  std::unique_lock<std::mutex> lk(mu_);
  if (!finished_) {
    return Status(StatusCode::kFailedPrecondition,
                  "Attempted to cleanup parallel upload state while it is "
                  "still in progress");
  }
  // Make sure that only one thread actually interacts with the deleter.
  if (deleter_) {
    cleanup_status_ = deleter_->ExecuteDelete();
    deleter_.reset();
  }
  return cleanup_status_;
}

void ParallelUploadStateImpl::Fail(Status status) {
  std::unique_lock<std::mutex> lk(mu_);
  assert(!status.ok());
  if (!res_) {
    // Preserve the first error.
    res_ = std::move(status);
  }
  if (num_unfinished_streams_ == 0) {
    AllStreamsFinished(lk);
  }
}

ParallelUploadPersistentState ParallelUploadStateImpl::ToPersistentState()
    const {
  std::unique_lock<std::mutex> lk(mu_);

  std::vector<ParallelUploadPersistentState::Stream> streams;
  for (auto const& stream : streams_) {
    streams.emplace_back(ParallelUploadPersistentState::Stream{
        stream.object_name, stream.resumable_session_id});
  }

  return ParallelUploadPersistentState{
      destination_object_name_, expected_generation_, std::move(streams)};
}

void ParallelUploadStateImpl::AllStreamsFinished(
    std::unique_lock<std::mutex>& lk) {
  if (!res_) {
    std::vector<ComposeSourceObject> to_compose;
    std::transform(
        streams_.begin(), streams_.end(), std::back_inserter(to_compose),
        [](StreamInfo const& stream) { return *stream.composition_arg; });
    // only execute ComposeMany if all the streams succeeded.
    lk.unlock();
    auto res = composer_(to_compose);
    lk.lock();
    if (res) {
      deleter_->Enable(true);
    }
    res_ = std::move(res);
  }
  // All done, wake up whomever is waiting.
  finished_ = true;
  auto promises_to_satisfy = std::move(res_promises_);
  lk.unlock();
  for (auto& promise : promises_to_satisfy) {
    promise.set_value(*res_);
  }
}

void ParallelUploadStateImpl::StreamFinished(
    std::size_t stream_idx, StatusOr<ResumableUploadResponse> const& response) {
  std::unique_lock<std::mutex> lk(mu_);
  if (streams_[stream_idx].finished) {
    return;
  }

  --num_unfinished_streams_;
  streams_[stream_idx].finished = true;
  if (!response) {
    // The upload failed, we don't event need to clean this up.
    if (!res_) {
      // Preserve the first error.
      res_ = response.status();
    }
  } else {
    ObjectMetadata const& metadata = *response->payload;
    deleter_->Add(metadata);
    streams_[stream_idx].composition_arg =
        ComposeSourceObject{metadata.name(), metadata.generation(), {}};
  }
  if (num_unfinished_streams_ > 0) {
    return;
  }
  AllStreamsFinished(lk);
}

void ParallelUploadStateImpl::StreamDestroyed(std::size_t stream_idx) {
  std::unique_lock<std::mutex> lk(mu_);
  if (!streams_[stream_idx].finished) {
    --num_unfinished_streams_;
    streams_[stream_idx].finished = true;
    // A stream which was not `Close`d is being destroyed. This means that
    // it had been `Suspend`ed, hence this parallel upload will never finish.
    if (!res_) {
      // Preserve the first error.
      res_ = Status(StatusCode::kCancelled, "A stream has been suspended.");
    }
    if (num_unfinished_streams_ == 0) {
      AllStreamsFinished(lk);
    }
  }
}

future<StatusOr<ObjectMetadata>> ParallelUploadStateImpl::WaitForCompletion()
    const {
  std::unique_lock<std::mutex> lk(mu_);

  if (finished_) {
    return make_ready_future(*res_);
  }
  res_promises_.emplace_back();
  auto res = res_promises_.back().get_future();
  return res;
}

ParallelUploadFileShard::~ParallelUploadFileShard() {
  // If the object wasn't moved-from (i.e. `state != nullptr`) and
  // `left_to_upload_ > 0` it means that the object is being destroyed without
  // actually uploading the file. We should make sure we don't create the
  // destination object and instead fail the whole operation.
  if (state_ != nullptr && left_to_upload_ > 0) {
    state_->Fail(Status(StatusCode::kCancelled,
                        "Shard destroyed before calling "
                        "ParallelUploadFileShard::Upload()."));
  }
}

Status ParallelUploadFileShard::Upload() {
  std::unique_ptr<char[]> buf(new char[upload_buffer_size_]);

  auto fail = [this](StatusCode error_code, std::string reason) {
    Status status(error_code, "ParallelUploadFileShard::Upload(" + file_name_ +
                                  "): " + reason);
    state_->Fail(status);
    ostream_.Close();
    return status;
  };
  std::ifstream istream(file_name_, std::ios::binary);
  if (!istream.good()) {
    return fail(StatusCode::kNotFound, "cannot open upload file source");
  }
  istream.seekg(offset_in_file_);
  if (!istream.good()) {
    return fail(StatusCode::kInternal, "file changed size during upload?");
  }
  while (left_to_upload_ > 0) {
    std::size_t const to_copy =
        std::min<std::uintmax_t>(left_to_upload_, upload_buffer_size_);
    istream.read(buf.get(), to_copy);
    if (!istream.good()) {
      return fail(StatusCode::kInternal, "cannot read from file source");
    }
    ostream_.write(buf.get(), to_copy);
    if (!ostream_.good()) {
      return Status(StatusCode::kInternal,
                    "Writing to output stream failed, look into whole parallel "
                    "upload status for more information");
    }
    left_to_upload_ -= to_copy;
  }
  ostream_.Close();
  if (ostream_.metadata()) {
    return Status();
  }
  return ostream_.metadata().status();
}

StatusOr<std::pair<std::string, std::int64_t>> ParseResumableSessionId(
    std::string const& session_id) {
  auto starts_with = [](std::string const& s, std::string const& prefix) {
    return s.substr(0, prefix.size()) == prefix;
  };
  Status invalid(StatusCode::kInternal,
                 "Not a valid parallel upload session ID");

  if (!starts_with(session_id, kSessionIdPrefix)) {
    return invalid;
  }
  std::string const object_and_gen =
      session_id.substr(std::string(kSessionIdPrefix).size());
  auto sep_pos = object_and_gen.find(':');
  if (sep_pos == std::string::npos) {
    return invalid;
  }
  std::string const object = object_and_gen.substr(0, sep_pos);
  std::string const generation_str = object_and_gen.substr(sep_pos + 1);

  char* endptr;
  std::int64_t const generation =
      std::strtoll(generation_str.c_str(), &endptr, 10);
  if (generation_str.c_str() == endptr || *endptr != '\0') {
    return invalid;
  }
  return std::make_pair(object, generation);
}

}  // namespace internal
}  // namespace STORAGE_CLIENT_NS
}  // namespace storage
}  // namespace cloud
}  // namespace google