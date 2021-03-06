#include "drake/systems/lcm/lcm_subscriber_system.h"

#include <iostream>
#include <utility>

#include "drake/common/drake_assert.h"
#include "drake/common/text_logging.h"
#include "drake/systems/framework/basic_vector.h"
#include "drake/systems/framework/output_port_value.h"

namespace drake {
namespace systems {
namespace lcm {

using drake::lcm::DrakeLcmInterface;
using std::make_unique;

namespace {
constexpr int kStateIndexMessage = 0;
constexpr int kStateIndexMessageCount = 1;
}  // namespace

// TODO(jwnimmer-tri) The "serializer xor translator" disjoint implementations
// within the method bodies below are not ideal, because of the code smell, and
// because it is likely confusing for users.  We should take further steps to
// make the Value<LcmMessage> port the primary output port, and find a better
// phrasing for the vector-valued output port for users.  For now though, this
// implementation serves as a transition point where we don't have to rewrite
// the old code yet, but still can supply the AbstractValue port for new code.

// TODO(siyuan): Probably should only have AbstractValue output, and
// change / move the translator for vector base values to a separate translator
// block.
LcmSubscriberSystem::LcmSubscriberSystem(
    const std::string& channel, const LcmAndVectorBaseTranslator* translator,
    std::unique_ptr<SerializerInterface> serializer,
    drake::lcm::DrakeLcmInterface* lcm)
    : channel_(channel),
      translator_(translator),
      serializer_(std::move(serializer)) {
  DRAKE_DEMAND((translator_ != nullptr) != (serializer_.get() != nullptr));
  DRAKE_DEMAND(lcm);

  lcm->Subscribe(channel_, this);
  if (translator_ != nullptr) {
    DeclareOutputPort(kVectorValued, translator_->get_vector_size());
  } else {
    DeclareAbstractOutputPort();
  }

  set_name(make_name(channel_));
}

LcmSubscriberSystem::LcmSubscriberSystem(
    const std::string& channel, std::unique_ptr<SerializerInterface> serializer,
    DrakeLcmInterface* lcm)
    : LcmSubscriberSystem(channel, nullptr, std::move(serializer), lcm) {}

LcmSubscriberSystem::LcmSubscriberSystem(
    const std::string& channel, const LcmAndVectorBaseTranslator& translator,
    DrakeLcmInterface* lcm)
    : LcmSubscriberSystem(channel, &translator, nullptr, lcm) {}

LcmSubscriberSystem::LcmSubscriberSystem(
    const std::string& channel,
    const LcmTranslatorDictionary& translator_dictionary,
    DrakeLcmInterface* lcm)
    : LcmSubscriberSystem(channel, translator_dictionary.GetTranslator(channel),
                          lcm) {}

LcmSubscriberSystem::~LcmSubscriberSystem() {}

void LcmSubscriberSystem::SetDefaultState(const Context<double>&,
                                          State<double>* state) const {
  if (translator_ != nullptr) {
    DRAKE_DEMAND(serializer_ == nullptr);
    ProcessMessageAndStoreToDiscreteState(state->get_mutable_discrete_state());
  } else {
    DRAKE_DEMAND(translator_ == nullptr);
    ProcessMessageAndStoreToAbstractState(state->get_mutable_abstract_state());
  }
}

// The decision to store the decoded message instead of the raw bytes in
// state is to avoid repeated decoding in DoCalcOutput for the latter case.
// However, this computational concern will not exist once caching is properly
// implemented and in place. Same for ProcessMessageAndStoreToAbstractState()
void LcmSubscriberSystem::ProcessMessageAndStoreToDiscreteState(
    DiscreteValues<double>* discrete_state) const {
  DRAKE_ASSERT(translator_ != nullptr);
  DRAKE_ASSERT(serializer_ == nullptr);

  std::lock_guard<std::mutex> lock(received_message_mutex_);
  if (!received_message_.empty()) {
    translator_->Deserialize(
        received_message_.data(), received_message_.size(),
        discrete_state->get_mutable_vector(kStateIndexMessage));
  }
  discrete_state->get_mutable_vector(kStateIndexMessageCount)
      ->SetAtIndex(0, received_message_count_);
}

void LcmSubscriberSystem::ProcessMessageAndStoreToAbstractState(
    AbstractValues* abstract_state) const {
  DRAKE_ASSERT(translator_ == nullptr);
  DRAKE_ASSERT(serializer_ != nullptr);

  std::lock_guard<std::mutex> lock(received_message_mutex_);
  if (!received_message_.empty()) {
    serializer_->Deserialize(
        received_message_.data(), received_message_.size(),
        &abstract_state->get_mutable_value(kStateIndexMessage));
  }
  abstract_state->get_mutable_value(kStateIndexMessageCount).
      GetMutableValue<int>() = received_message_count_;
}

void LcmSubscriberSystem::DoCalcNextUpdateTime(
    const Context<double>& context, UpdateActions<double>* events) const {
  // Gets the last message count from either abstract state or discrete state.
  int last_message_count;
  if (translator_ == nullptr) {
    DRAKE_ASSERT(serializer_ != nullptr);
    last_message_count =
        context.get_abstract_state<int>(kStateIndexMessageCount);
  } else {
    DRAKE_ASSERT(serializer_ == nullptr);
    last_message_count = static_cast<int>(
        context.get_discrete_state(kStateIndexMessageCount)->GetAtIndex(0));
  }

  std::unique_lock<std::mutex> lock(received_message_mutex_);
  // Has a new message. Schedule an update event.
  if (last_message_count != received_message_count_) {
    // TODO(siyuan): should be context.get_time() once #5725 is resolved.
    events->time = context.get_time() + 0.0001;
    DiscreteEvent<double> event;
    if (translator_ == nullptr) {
      event.action =
          DiscreteEvent<double>::ActionType::kUnrestrictedUpdateAction;
    } else {
      event.action = DiscreteEvent<double>::ActionType::kDiscreteUpdateAction;
    }
    events->events.push_back(event);
  } else {
    // Use base class' implementation.
    LeafSystem<double>::DoCalcNextUpdateTime(context, events);
  }
}

std::unique_ptr<DiscreteValues<double>>
LcmSubscriberSystem::AllocateDiscreteState() const {
  // Only make discrete states if we are outputing vector values.
  if (translator_ != nullptr) {
    DRAKE_DEMAND(serializer_ == nullptr);
    std::vector<std::unique_ptr<BasicVector<double>>> discrete_state_vec(2);
    discrete_state_vec[kStateIndexMessage] =
        this->AllocateOutputVector(this->get_output_port(0));
    discrete_state_vec[kStateIndexMessageCount] =
        std::make_unique<BasicVector<double>>(1);
    return std::make_unique<DiscreteValues<double>>(
        std::move(discrete_state_vec));
  }
  DRAKE_DEMAND(serializer_ != nullptr);
  return LeafSystem<double>::AllocateDiscreteState();
}

std::unique_ptr<AbstractValues> LcmSubscriberSystem::AllocateAbstractState()
    const {
  // Only make abstract states if we are outputing abstract message.
  if (serializer_ != nullptr) {
    DRAKE_DEMAND(translator_ == nullptr);
    std::vector<std::unique_ptr<systems::AbstractValue>> abstract_vals(2);
    abstract_vals[kStateIndexMessage] =
        this->AllocateOutputAbstract(this->get_output_port(0));
    abstract_vals[kStateIndexMessageCount] = AbstractValue::Make<int>(0);
    return std::make_unique<systems::AbstractValues>(std::move(abstract_vals));
  }
  DRAKE_DEMAND(translator_ != nullptr);
  return LeafSystem<double>::AllocateAbstractState();
}

std::string LcmSubscriberSystem::make_name(const std::string& channel) {
  return "LcmSubscriberSystem(" + channel + ")";
}

const std::string& LcmSubscriberSystem::get_channel_name() const {
  return channel_;
}

void LcmSubscriberSystem::DoCalcOutput(const Context<double>& context,
                                       SystemOutput<double>* output) const {
  DRAKE_ASSERT((translator_ != nullptr) != (serializer_.get() != nullptr));

  if (translator_ != nullptr) {
    BasicVector<double>* const output_vector = output->GetMutableVectorData(0);
    DRAKE_ASSERT(output_vector != nullptr);

    output_vector->SetFrom(*context.get_discrete_state(kStateIndexMessage));
  } else {
    AbstractValue* const output_value = output->GetMutableData(0);
    DRAKE_ASSERT(output_value != nullptr);

    output_value->SetFrom(
        context.get_abstract_state()->get_value(kStateIndexMessage));
  }
}

// This is only called if our output port is vector-valued.
std::unique_ptr<BasicVector<double>> LcmSubscriberSystem::AllocateOutputVector(
    const OutputPortDescriptor<double>& descriptor) const {
  DRAKE_DEMAND(descriptor.get_index() == 0);
  DRAKE_DEMAND(descriptor.get_data_type() == kVectorValued);
  DRAKE_DEMAND(translator_ != nullptr);
  DRAKE_DEMAND(serializer_ == nullptr);
  auto result = translator_->AllocateOutputVector();
  if (result) {
    return result;
  }
  return LeafSystem<double>::AllocateOutputVector(descriptor);
}

// This is only called if our output port is abstract-valued.
std::unique_ptr<systems::AbstractValue>
LcmSubscriberSystem::AllocateOutputAbstract(
    const OutputPortDescriptor<double>& descriptor) const {
  DRAKE_DEMAND(descriptor.get_index() == 0);
  DRAKE_DEMAND(descriptor.get_data_type() == kAbstractValued);
  DRAKE_DEMAND(translator_ == nullptr);
  DRAKE_DEMAND(serializer_ != nullptr);
  return serializer_->CreateDefaultValue();
}

void LcmSubscriberSystem::HandleMessage(const std::string& channel,
                                        const void* message_buffer,
                                        int message_size) {
  SPDLOG_TRACE(drake::log(), "Receiving LCM {} message", channel);

  if (channel == channel_) {
    const uint8_t* const rbuf_begin =
        reinterpret_cast<const uint8_t*>(message_buffer);
    const uint8_t* const rbuf_end = rbuf_begin + message_size;
    std::lock_guard<std::mutex> lock(received_message_mutex_);
    received_message_.clear();
    received_message_.insert(received_message_.begin(), rbuf_begin, rbuf_end);

    received_message_count_++;
    received_message_condition_variable_.notify_all();
  } else {
    std::cerr << "LcmSubscriberSystem: HandleMessage: WARNING: Received a "
              << "message for channel \"" << channel
              << "\" instead of channel \"" << channel_ << "\". Ignoring it."
              << std::endl;
  }
}

int LcmSubscriberSystem::WaitForMessage(int old_message_count) const {
  // The message buffer and counter are updated in HandleMessage(), which is
  // a callback function invoked by a different thread owned by the
  // drake::lcm::DrakeLcmInterface instance passed to the constructor. Thus,
  // for thread safety, these need to be properly protected by a mutex.
  std::unique_lock<std::mutex> lock(received_message_mutex_);

  // This while loop is necessary to guard for spurious wakeup:
  // https://en.wikipedia.org/wiki/Spurious_wakeup
  while (old_message_count == received_message_count_)
    // When wait returns, lock is atomically acquired. So it's thread safe to
    // read received_message_count_.
    received_message_condition_variable_.wait(lock);
  int new_message_count = received_message_count_;
  lock.unlock();

  return new_message_count;
}

const LcmAndVectorBaseTranslator& LcmSubscriberSystem::get_translator() const {
  DRAKE_DEMAND(translator_ != nullptr);
  return *translator_;
}

}  // namespace lcm
}  // namespace systems
}  // namespace drake
