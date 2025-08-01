#include "error.h"
#include "serialize.h"

#include <yt/yt/core/concurrency/fls.h>
#include <yt/yt/core/concurrency/scheduler_api.h>

#include <yt/yt/core/net/local_address.h>

#include <yt/yt/core/misc/collection_helpers.h>
#include <yt/yt/core/misc/protobuf_helpers.h>

#include <yt/yt/core/tracing/trace_context.h>

#include <yt/yt/core/yson/tokenizer.h>
#include <yt/yt/core/yson/protobuf_helpers.h>

#include <yt/yt/core/ytree/attributes.h>
#include <yt/yt/core/ytree/fluent.h>

#include <yt/yt_proto/yt/core/misc/proto/error.pb.h>

#include <library/cpp/yt/global/variable.h>

#include <library/cpp/yt/misc/global.h>

namespace NYT {

using namespace NTracing;
using namespace NYTree;
using namespace NYson;

using NYT::FromProto;
using NYT::ToProto;

////////////////////////////////////////////////////////////////////////////////

constexpr TStringBuf OriginalErrorDepthAttribute = "original_error_depth";

bool TErrorCodicils::Initialized_ = false;

////////////////////////////////////////////////////////////////////////////////

namespace NDetail {

namespace {

struct TExtensionData
{
    NConcurrency::TFiberId Fid = NConcurrency::InvalidFiberId;
    TStringBuf HostName;
    TTraceId TraceId = InvalidTraceId;
    TSpanId SpanId = InvalidSpanId;
};

TOriginAttributes::TErasedExtensionData Encode(TExtensionData data)
{
    return TOriginAttributes::TErasedExtensionData{data};
}

TExtensionData Decode(const TOriginAttributes::TErasedExtensionData& storage)
{
    return storage.AsConcrete<TExtensionData>();
}

void TryExtractHost(const TOriginAttributes& attributes)
{
    if (attributes.Host || !attributes.ExtensionData) {
        return;
    }

    auto [
        fid,
        name,
        traceId,
        spanId
    ] = Decode(*attributes.ExtensionData);

    attributes.Host = name
        ? TStringBuf(name)
        : TStringBuf{};
}

////////////////////////////////////////////////////////////////////////////////

bool HasHost(const TOriginAttributes& attributes) noexcept
{
    TryExtractHost(attributes);
    return attributes.Host.operator bool();
}

TStringBuf GetHost(const TOriginAttributes& attributes) noexcept
{
    TryExtractHost(attributes);
    return attributes.Host;
}

NConcurrency::TFiberId GetFid(const TOriginAttributes& attributes) noexcept
{
    if (attributes.ExtensionData.has_value()) {
        return Decode(*attributes.ExtensionData).Fid;
    }
    return NConcurrency::InvalidFiberId;
}

NTracing::TTraceId GetTraceId(const TOriginAttributes& attributes) noexcept
{
    if (attributes.ExtensionData.has_value()) {
        return Decode(*attributes.ExtensionData).TraceId;
    }
    return InvalidTraceId;
}

NTracing::TSpanId GetSpanId(const TOriginAttributes& attributes) noexcept
{
    if (attributes.ExtensionData.has_value()) {
        return Decode(*attributes.ExtensionData).SpanId;
    }
    return InvalidSpanId;
}

bool HasTracingAttributes(const TOriginAttributes& attributes) noexcept
{
    return GetTraceId(attributes) != InvalidTraceId;
}

void UpdateTracingAttributes(TOriginAttributes* attributes, const NTracing::TTracingAttributes& tracingAttributes)
{
    if (attributes->ExtensionData.has_value()) {
        auto ext = Decode(*attributes->ExtensionData);
        attributes->ExtensionData.emplace(Encode(TExtensionData{
            .Fid = ext.Fid,
            .HostName = ext.HostName,
            .TraceId = tracingAttributes.TraceId,
            .SpanId = tracingAttributes.SpanId,
        }));
        return;
    }

    attributes->ExtensionData.emplace(Encode(TExtensionData{
        .TraceId = tracingAttributes.TraceId,
        .SpanId = tracingAttributes.SpanId,
    }));
}

////////////////////////////////////////////////////////////////////////////////

TOriginAttributes::TErasedExtensionData GetExtensionDataOverride()
{
    TExtensionData result;
    result.Fid = NConcurrency::GetCurrentFiberId();
    result.HostName = NNet::GetLocalHostNameRaw();

    if (const auto* traceContext = NTracing::TryGetCurrentTraceContext()) {
        result.TraceId = traceContext->GetTraceId();
        result.SpanId = traceContext->GetSpanId();
    }

    return TOriginAttributes::TErasedExtensionData{result};
}

TString FormatOriginOverride(const TOriginAttributes& attributes)
{
    TryExtractHost(attributes);
    return Format("%v (pid %v, thread %v, fid %x)",
        attributes.Host,
        attributes.Pid,
        MakeFormatterWrapper([&] (auto* builder) {
            auto threadName = attributes.ThreadName.ToStringBuf();
            if (threadName.empty()) {
                FormatValue(builder, attributes.Tid, "v");
                return;
            }
            FormatValue(builder, threadName, "v");
        }),
        GetFid(attributes));
}

TOriginAttributes ExtractFromDictionaryOverride(TErrorAttributes* attributes)
{
    auto result = NYT::NDetail::ExtractFromDictionaryDefault(attributes);

    TExtensionData ext;

    if (attributes) {
        static const TString FidKey("fid");
        ext.Fid = attributes->GetAndRemove<NConcurrency::TFiberId>(FidKey, NConcurrency::InvalidFiberId);

        static const TString TraceIdKey("trace_id");
        ext.TraceId = attributes->GetAndRemove<NTracing::TTraceId>(TraceIdKey, NTracing::InvalidTraceId);

        static const TString SpanIdKey("span_id");
        ext.SpanId = attributes->GetAndRemove<NTracing::TSpanId>(SpanIdKey, NTracing::InvalidSpanId);
    }

    result.ExtensionData = Encode(ext);
    return result;
}

} // namespace

////////////////////////////////////////////////////////////////////////////////

void EnableErrorOriginOverrides()
{
    static NGlobal::TVariable<std::byte> getExtensionDataOverride{
        NYT::NDetail::GetExtensionDataTag,
        +[] () noexcept {
            return NGlobal::TErasedStorage{&GetExtensionDataOverride};
        }};

    static NGlobal::TVariable<std::byte> formatOriginOverride{
        NYT::NDetail::FormatOriginTag,
        +[] () noexcept {
            return NGlobal::TErasedStorage{&FormatOriginOverride};
        }};

    static NGlobal::TVariable<std::byte> extractFromDictionaryOverride{
        NYT::NDetail::ExtractFromDictionaryTag,
        +[] () noexcept {
            return NGlobal::TErasedStorage{&ExtractFromDictionaryOverride};
        }};

    getExtensionDataOverride.Get();
    formatOriginOverride.Get();
    extractFromDictionaryOverride.Get();
}

} // namespace NDetail

////////////////////////////////////////////////////////////////////////////////

bool HasHost(const TError& error) noexcept
{
    if (auto* attributes = error.MutableOriginAttributes()) {
        return NYT::NDetail::HasHost(*attributes);
    }
    return false;
}

TStringBuf GetHost(const TError& error) noexcept
{
    if (auto* attributes = error.MutableOriginAttributes()) {
        return NYT::NDetail::GetHost(*attributes);
    }
    return {};
}

NConcurrency::TFiberId GetFid(const TError& error) noexcept
{
    if (auto* attributes = error.MutableOriginAttributes()) {
        return NYT::NDetail::GetFid(*attributes);
    }
    return NConcurrency::InvalidFiberId;
}

bool HasTracingAttributes(const TError& error) noexcept
{
    if (auto* attributes = error.MutableOriginAttributes()) {
        return NYT::NDetail::HasTracingAttributes(*attributes);
    }
    return false;
}

NTracing::TTraceId GetTraceId(const TError& error) noexcept
{
    if (auto* attributes = error.MutableOriginAttributes()) {
        return NYT::NDetail::GetTraceId(*attributes);
    }
    return NTracing::InvalidTraceId;
}

NTracing::TSpanId GetSpanId(const TError& error) noexcept
{
    if (auto* attributes = error.MutableOriginAttributes()) {
        return NYT::NDetail::GetSpanId(*attributes);
    }
    return NTracing::InvalidSpanId;
}

void SetTracingAttributes(TError* error, const NTracing::TTracingAttributes& attributes)
{
    auto* originAttributes = error->MutableOriginAttributes();

    if (!originAttributes) {
        return;
    }

    NYT::NDetail::UpdateTracingAttributes(originAttributes, attributes);
}

////////////////////////////////////////////////////////////////////////////////

namespace {

// Errors whose depth exceeds |ErrorSerializationDepthLimit| are serialized
// as children of their ancestor on depth |ErrorSerializationDepthLimit - 1|.
[[maybe_unused]]
void SerializeInnerErrors(TFluentMap fluent, const TError& error, int depth)
{
    if (depth >= ErrorSerializationDepthLimit) {
        // Ignore deep inner errors.
        return;
    }

    auto visit = [&] (auto fluent, const TError& error, int depth) {
        fluent
            .Item().Do([&] (auto fluent) {
                Serialize(error, fluent.GetConsumer(), /*valueProduce*/ nullptr, depth);
            });
    };

    fluent
        .Item("inner_errors").DoListFor(error.InnerErrors(), [&] (auto fluent, const TError& innerError) {
            if (depth < ErrorSerializationDepthLimit - 1) {
                visit(fluent, innerError, depth + 1);
            } else {
                YT_VERIFY(depth == ErrorSerializationDepthLimit - 1);
                TraverseError(
                    innerError,
                    [&] (const TError& e, int depth) {
                        visit(fluent, e, depth);
                    },
                    depth + 1);
            }
        });
}

} // namespace

////////////////////////////////////////////////////////////////////////////////

void Serialize(
    const TErrorCode& errorCode,
    IYsonConsumer* consumer)
{
    consumer->OnInt64Scalar(static_cast<int>(errorCode));
}

void Deserialize(
    TErrorCode& errorCode,
    const NYTree::INodePtr& node)
{
    errorCode = TErrorCode(node->GetValue<int>());
}

void Serialize(
    const TError& error,
    IYsonConsumer* consumer,
    const std::function<void(IYsonConsumer*)>* valueProducer,
    int depth)
{
    BuildYsonFluently(consumer)
        .BeginMap()
            .Item("code").Value(error.GetCode())
            .Item("message").Value(error.GetMessage())
            .Item("attributes").DoMap([&] (auto fluent) {
                if (error.HasOriginAttributes()) {
                    fluent
                        .Item("pid").Value(error.GetPid())
                        .Item("tid").Value(error.GetTid())
                        .Item("thread").Value(error.GetThreadName())
                        .Item("fid").Value(GetFid(error));
                }
                if (HasHost(error)) {
                    fluent
                        .Item("host").Value(GetHost(error));
                }
                if (error.HasDatetime()) {
                    fluent
                        .Item("datetime").Value(error.GetDatetime());
                }
                if (HasTracingAttributes(error)) {
                    fluent
                        .Item("trace_id").Value(GetTraceId(error))
                        .Item("span_id").Value(GetSpanId(error));
                }
                if (depth > ErrorSerializationDepthLimit && !error.Attributes().Contains(OriginalErrorDepthAttribute)) {
                    fluent
                        .Item(OriginalErrorDepthAttribute).Value(depth);
                }
                for (const auto& [key, value] : error.Attributes().ListPairs()) {
                    fluent
                        .Item(key).Value(NYson::TYsonString(value));
                }
            })
            .DoIf(!error.InnerErrors().empty(), [&] (auto fluent) {
                SerializeInnerErrors(fluent, error, depth);
            })
            .DoIf(valueProducer != nullptr, [&] (auto fluent) {
                auto* consumer = fluent.GetConsumer();
                // NB: We are forced to deal with a bare consumer here because
                // we can't use void(TFluentMap) in a function signature as it
                // will lead to the inclusion of fluent.h in error.h and a cyclic
                // inclusion error.h -> fluent.h -> callback.h -> error.h
                consumer->OnKeyedItem(TStringBuf("value"));
                (*valueProducer)(consumer);
            })
        .EndMap();
}

void Deserialize(TError& error, const NYTree::INodePtr& node)
{
    error = {};

    auto mapNode = node->AsMap();

    static const TString CodeKey("code");
    auto code = TErrorCode(mapNode->GetChildValueOrThrow<i64>(CodeKey));
    if (code == NYT::EErrorCode::OK) {
        return;
    }

    error.SetCode(code);

    static const TString MessageKey("message");
    error.SetMessage(mapNode->GetChildValueOrThrow<TString>(MessageKey));

    static const TString AttributesKey("attributes");
    auto children = mapNode->GetChildOrThrow(AttributesKey)->AsMap()->GetChildren();

    for (const auto& [key, value] : children) {
        // NB(arkady-e1ppa): Serialization may add some attributes in normal yson
        // format (in legacy versions) thus we have to reconvert them into the
        // text ones in order to make sure that everything is in the text format.
        error <<= TErrorAttribute(key, ConvertToYsonString(value));
    }

    error.UpdateOriginAttributes();

    static const TString InnerErrorsKey("inner_errors");
    if (auto innerErrorsNode = mapNode->FindChild(InnerErrorsKey)) {
        for (const auto& innerErrorNode : innerErrorsNode->AsList()->GetChildren()) {
            error.MutableInnerErrors()->push_back(ConvertTo<TError>(innerErrorNode));
        }
    }
}

void Deserialize(TError& error, NYson::TYsonPullParserCursor* cursor)
{
    Deserialize(error, ExtractTo<INodePtr>(cursor));
}

////////////////////////////////////////////////////////////////////////////////

void ToProto(NYT::NProto::TError* protoError, const TError& error)
{
    if (error.IsOK()) {
        protoError->set_code(ToProto(NYT::EErrorCode::OK));
        protoError->clear_message();
        return;
    }

    protoError->set_code(error.GetCode());
    protoError->set_message(error.GetMessage());

    protoError->clear_attributes();
    if (error.HasAttributes()) {
        auto* protoAttributes = protoError->mutable_attributes();

        protoAttributes->Clear();
        auto pairs = error.Attributes().ListPairs();
        std::sort(pairs.begin(), pairs.end(), [] (const auto& lhs, const auto& rhs) {
            return lhs.first < rhs.first;
        });
        protoAttributes->mutable_attributes()->Reserve(pairs.size());
        for (const auto& [key, value] : pairs) {
            auto* protoAttribute = protoAttributes->add_attributes();
            protoAttribute->set_key(key);
            protoAttribute->set_value(value);
        }
    }

    auto addAttribute = [&] (const TString& key, const auto& value) {
        auto* protoItem = protoError->mutable_attributes()->add_attributes();
        protoItem->set_key(key);
        protoItem->set_value(ToProto(ConvertToYsonString(value)));
    };

    if (error.HasOriginAttributes()) {
        static const TString PidKey("pid");
        addAttribute(PidKey, error.GetPid());

        static const TString TidKey("tid");
        addAttribute(TidKey, error.GetTid());

        static const TString ThreadName("thread");
        addAttribute(ThreadName, error.GetThreadName());

        static const TString FidKey("fid");
        addAttribute(FidKey, GetFid(error));
    }

    if (HasHost(error)) {
        static const TString HostKey("host");
        addAttribute(HostKey, GetHost(error));
    }

    if (error.HasDatetime()) {
        static const TString DatetimeKey("datetime");
        addAttribute(DatetimeKey, error.GetDatetime());
    }

    if (HasTracingAttributes(error)) {
        static const TString TraceIdKey("trace_id");
        addAttribute(TraceIdKey, GetTraceId(error));

        static const TString SpanIdKey("span_id");
        addAttribute(SpanIdKey, GetSpanId(error));
    }

    protoError->clear_inner_errors();
    for (const auto& innerError : error.InnerErrors()) {
        ToProto(protoError->add_inner_errors(), innerError);
    }
}

void FromProto(TError* error, const NYT::NProto::TError& protoError)
{
    *error = {};

    if (protoError.code() == static_cast<int>(NYT::EErrorCode::OK)) {
        return;
    }

    error->SetCode(TErrorCode(protoError.code()));
    error->SetMessage(FromProto<TString>(protoError.message()));
    if (protoError.has_attributes()) {
        for (const auto& protoAttribute : protoError.attributes().attributes()) {
            // NB(arkady-e1ppa): Again for compatibility reasons we have to reconvert stuff
            // here as well.
            auto key = FromProto<TString>(protoAttribute.key());
            auto value = FromProto<TString>(protoAttribute.value());
            (*error) <<= TErrorAttribute(key, TYsonString(value));
        }
        error->UpdateOriginAttributes();
    }
    *error->MutableInnerErrors() = FromProto<std::vector<TError>>(protoError.inner_errors());
}

////////////////////////////////////////////////////////////////////////////////

void TErrorSerializer::Save(TStreamSaveContext& context, const TErrorCode& errorCode)
{
    NYT::Save(context, static_cast<int>(errorCode));
}

void TErrorSerializer::Load(TStreamLoadContext& context, TErrorCode& errorCode)
{
    int value = 0;
    NYT::Load(context, value);
    errorCode = TErrorCode{value};
}

////////////////////////////////////////////////////////////////////////////////

void TErrorSerializer::Save(TStreamSaveContext& context, const TError& error)
{
    using NYT::Save;

    if (error.IsOK()) {
        // Fast path.
        Save(context, TErrorCode(NYT::EErrorCode::OK)); // code
        Save(context, TStringBuf());                    // message
        Save(context, IAttributeDictionaryPtr());       // attributes
        Save(context, std::vector<TError>());           // inner errors
        return;
    }

    Save(context, error.GetCode());
    Save(context, error.GetMessage());

    // Cf. TAttributeDictionaryValueSerializer.
    auto attributePairs = error.Attributes().ListPairs();
    size_t attributeCount = attributePairs.size();
    if (HasHost(error)) {
        attributeCount += 1;
    }
    if (error.HasOriginAttributes()) {
        attributeCount += 4;
    }
    if (error.HasDatetime()) {
        attributeCount += 1;
    }
    if (HasTracingAttributes(error)) {
        attributeCount += 2;
    }

    if (attributeCount > 0) {
        // Cf. TAttributeDictionaryRefSerializer.
        Save(context, true);

        TSizeSerializer::Save(context, attributeCount);

        auto saveAttribute = [&] (const TString& key, const auto& value) {
            Save(context, key);
            Save(context, ConvertToYsonString(value));
        };

        if (HasHost(error)) {
            static const TString HostKey("host");
            saveAttribute(HostKey, GetHost(error));
        }

        if (error.HasOriginAttributes()) {
            static const TString PidKey("pid");
            saveAttribute(PidKey, error.GetPid());

            static const TString TidKey("tid");
            saveAttribute(TidKey, error.GetTid());

            static const TString ThreadNameKey("thread");
            saveAttribute(ThreadNameKey, error.GetThreadName());

            static const TString FidKey("fid");
            saveAttribute(FidKey, GetFid(error));
        }

        if (error.HasDatetime()) {
            static const TString DatetimeKey("datetime");
            saveAttribute(DatetimeKey, error.GetDatetime());
        }

        if (HasTracingAttributes(error)) {
            static const TString TraceIdKey("trace_id");
            saveAttribute(TraceIdKey, GetTraceId(error));

            static const TString SpanIdKey("span_id");
            saveAttribute(SpanIdKey, GetSpanId(error));
        }

        std::sort(attributePairs.begin(), attributePairs.end(), [] (const auto& lhs, const auto& rhs) {
            return lhs.first < rhs.first;
        });
        for (const auto& [key, value] : attributePairs) {
            // NB(arkady-e1ppa): For the sake of compatibility we keep the old
            // serialization format.
            Save(context, TString(key));
            Save(context, NYson::TYsonString(value));
        }
    } else {
        Save(context, false);
    }

    Save(context, error.InnerErrors());
}

void TErrorSerializer::Load(TStreamLoadContext& context, TError& error)
{
    using NYT::Load;

    error = {};

    auto code = Load<TErrorCode>(context);
    auto message = Load<TString>(context);

    if (Load<bool>(context)) {
        size_t size = TSizeSerializer::Load(context);
        for (size_t index = 0; index < size; ++index) {
            auto key = Load<TString>(context);
            auto value = Load<TYsonString>(context);
            error <<= TErrorAttribute(key, value);
        }
    }

    auto innerErrors = Load<std::vector<TError>>(context);

    if (code == NYT::EErrorCode::OK) {
        // Fast path.
        return;
    }

    error.SetCode(code);
    error.UpdateOriginAttributes();
    error.SetMessage(std::move(message));
    *error.MutableInnerErrors() = std::move(innerErrors);
}

////////////////////////////////////////////////////////////////////////////////

static YT_DEFINE_GLOBAL(NConcurrency::TFlsSlot<TErrorCodicils>, ErrorCodicilsSlot);

TErrorCodicils::TGuard::~TGuard()
{
    TErrorCodicils::GetOrCreate().Set(std::move(Key_), std::move(OldGetter_));
}

TErrorCodicils::TGuard::TGuard(
    std::string key,
    TGetter oldGetter)
    : Key_(std::move(key))
    , OldGetter_(std::move(oldGetter))
{ }

void TErrorCodicils::Initialize()
{
    if (Initialized_) {
        // Multiple calls are OK.
        return;
    }
    Initialized_ = true;

    ErrorCodicilsSlot(); // Warm up the slot.
    TError::RegisterEnricher([] (TError* error) {
        if (auto* codicils = TErrorCodicils::MaybeGet()) {
            codicils->Apply(*error);
        }
    });
}

TErrorCodicils& TErrorCodicils::GetOrCreate()
{
    return *ErrorCodicilsSlot().GetOrCreate();
}

TErrorCodicils* TErrorCodicils::MaybeGet()
{
    return ErrorCodicilsSlot().MaybeGet();
}

std::optional<std::string> TErrorCodicils::MaybeEvaluate(const std::string& key)
{
    auto* instance = MaybeGet();
    if (!instance) {
        return {};
    }

    auto getter = instance->Get(key);
    if (!getter) {
        return {};
    }

    return getter();
}

auto TErrorCodicils::Guard(std::string key, TGetter getter) -> TGuard
{
    auto& instance = GetOrCreate();
    auto [it, added] = instance.Getters_.try_emplace(key, getter);
    TGetter oldGetter;
    if (!added) {
        oldGetter = std::move(it->second);
        it->second = std::move(getter);
    }
    return TGuard(std::move(key), std::move(oldGetter));
}

void TErrorCodicils::Apply(TError& error) const
{
    for (const auto& [key, getter] : Getters_) {
        error <<= TErrorAttribute(key, getter());
    }
}

void TErrorCodicils::Set(std::string key, TGetter getter)
{
    // We could enforce Initialized_, but that could make an error condition worse at runtime.
    // Instead, let's keep enrichment optional.
    if (getter) {
        Getters_.insert_or_assign(std::move(key), std::move(getter));
    } else {
        Getters_.erase(key);
    }
}

auto TErrorCodicils::Get(const std::string& key) const -> TGetter
{
    return GetOrDefault(Getters_, key);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT
