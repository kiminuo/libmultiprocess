// Copyright (c) 2019 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <boost/optional.hpp>
#include <capnp/schema-parser.h>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <vector>

#define PROXY_BIN "mpgen"
#define PROXY_DECL "mp/proxy.h"
#define PROXY_TYPES "mp/proxy-types.h"

constexpr uint64_t NAMESPACE_ANNOTATION_ID = 0xb9c6f99ebf805f2cull; // From c++.capnp
constexpr uint64_t WRAP_ANNOTATION_ID = 0xe6f46079b7b1405eull;      // From proxy.capnp
constexpr uint64_t COUNT_ANNOTATION_ID = 0xd02682b319f69b38ull;     // From proxy.capnp
constexpr uint64_t EXCEPTION_ANNOTATION_ID = 0x996a183200992f88ull; // From proxy.capnp
constexpr uint64_t NAME_ANNOTATION_ID = 0xb594888f63f4dbb9ull;      // From proxy.capnp
constexpr uint64_t SKIP_ANNOTATION_ID = 0x824c08b82695d8ddull;      // From proxy.capnp

template <typename Reader>
boost::optional<capnp::schema::Value::Reader> GetAnnotation(const Reader& reader, uint64_t id)
{
    for (const auto annotation : reader.getAnnotations()) {
        if (annotation.getId() == id) return annotation.getValue();
    }
    return {};
}

using CharSlice = kj::ArrayPtr<const char>;

// Overload for any type with a string .begin(), like kj::StringPtr and kj::ArrayPtr<char>.
template <class OutputStream, class Array, const char* Enable = decltype(std::declval<Array>().begin())()>
OutputStream& operator<<(OutputStream& os, const Array& array)
{
    os.write(array.begin(), array.size());
    return os;
}

struct Format
{
    template <typename Value>
    Format& operator<<(Value&& value)
    {
        m_os << value;
        return *this;
    }
    operator std::string() { return m_os.str(); }
    std::ostringstream m_os;
};

std::string Cap(kj::StringPtr str)
{
    std::string result = str;
    if (!result.empty() && 'a' <= result[0] && result[0] <= 'z') result[0] -= 'a' - 'A';
    return result;
}

bool BoxedType(const ::capnp::Type& type)
{
    return !(type.isVoid() || type.isBool() || type.isInt8() || type.isInt16() || type.isInt32() || type.isInt64() ||
             type.isUInt8() || type.isUInt16() || type.isUInt32() || type.isUInt64() || type.isFloat32() ||
             type.isFloat64() || type.isEnum());
}

void Generate(kj::StringPtr output_stem, kj::StringPtr input_schema, kj::ArrayPtr<const kj::StringPtr> import_paths)
{
    capnp::SchemaParser parser;
    auto file_schema = parser.parseDiskFile(input_schema, input_schema, import_paths);

    const std::string stem = output_stem;
    std::ofstream cpp_server(stem + ".capnp.proxy-server.c++");
    cpp_server << "// Generated by " PROXY_BIN " from " << input_schema << "\n\n";
    cpp_server << "#include <" << stem << ".capnp.proxy-types.h>\n";
    cpp_server << "#include <" << PROXY_TYPES << ">\n\n";
    cpp_server << "namespace mp {\n";

    std::ofstream cpp_client(stem + ".capnp.proxy-client.c++");
    cpp_client << "// Generated by " PROXY_BIN " from " << input_schema << "\n\n";
    cpp_client << "#include <" << stem << ".capnp.proxy-types.h>\n";
    cpp_client << "#include <" << PROXY_TYPES << ">\n\n";
    cpp_client << "namespace mp {\n";

    std::ofstream cpp_types(stem + ".capnp.proxy-types.c++");
    cpp_types << "// Generated by " PROXY_BIN " from " << input_schema << "\n\n";
    cpp_types << "#include <" << stem << ".capnp.proxy-types.h>\n";
    cpp_types << "#include <" << PROXY_TYPES << ">\n\n";
    cpp_types << "namespace mp {\n";

    std::string guard = stem;
    std::transform(guard.begin(), guard.end(), guard.begin(), [](unsigned char c) {
        return ('0' <= c && c <= '9') ? c : ('A' <= c && c <= 'Z') ? c : ('a' <= c && c <= 'z') ? c - 'a' + 'A' : '_';
    });

    std::ofstream inl(stem + ".capnp.proxy-types.h");
    inl << "// Generated by " PROXY_BIN " from " << input_schema << "\n\n";
    inl << "#ifndef " << guard << "_CAPNP_PROXY_TYPES_H\n";
    inl << "#define " << guard << "_CAPNP_PROXY_TYPES_H\n\n";
    inl << "#include <" << stem << ".capnp.proxy.h>\n";
    inl << "#include <" << stem << "-types.h>\n\n";
    inl << "namespace mp {\n";

    std::ofstream h(stem + ".capnp.proxy.h");
    h << "// Generated by " PROXY_BIN " from " << input_schema << "\n\n";
    h << "#ifndef " << guard << "_CAPNP_PROXY_H\n";
    h << "#define " << guard << "_CAPNP_PROXY_H\n\n";
    h << "#include <" << stem << ".h>\n";
    h << "#include <" << stem << ".capnp.h>\n";
    h << "#include <" << PROXY_DECL << ">\n\n";
    h << "namespace mp {\n";

    kj::StringPtr message_namespace;
    if (auto value = GetAnnotation(file_schema.getProto(), NAMESPACE_ANNOTATION_ID)) {
        message_namespace = value->getText();
    }

    std::string output_name = output_stem;
    size_t output_slash = output_name.rfind("/");
    if (output_slash != std::string::npos) {
        output_name.erase(0, output_slash + 1);
    }

    std::ostringstream methods;
    std::set<kj::StringPtr> accessors_done;
    std::ostringstream accessors;
    std::ostringstream dec;
    std::ostringstream def_server;
    std::ostringstream def_client;
    std::ostringstream def_types;

    auto add_accessor = [&](kj::StringPtr name) {
        if (!accessors_done.insert(name).second) return;
        std::string cap = Cap(name);
        accessors << "struct " << cap << "\n";
        accessors << "{\n";
        accessors << "    template<typename S> static auto get(S&& s) -> AUTO_RETURN(s.get" << cap << "())\n";
        accessors << "    template<typename S> static bool has(S&& s) { return s.has" << cap << "(); }\n";
        accessors << "    template<typename S, typename A> static void set(S&& s, A&& a) { s.set" << cap
                  << "(std::forward<A>(a)); }\n";
        accessors << "    template<typename S, typename... A> static auto init(S&& s, A&&... a) -> AUTO_RETURN(s.init"
                  << cap << "(std::forward<A>(a)...))\n";
        accessors << "    template<typename S> static bool getWant(S&& s) { return s.getWant" << cap << "(); }\n";
        accessors << "    template<typename S> static void setWant(S&& s) { s.setWant" << cap << "(true); }\n";
        accessors << "    template<typename S> static bool getHas(S&& s) { return s.getHas" << cap << "(); }\n";
        accessors << "    template<typename S> static void setHas(S&& s) { s.setHas" << cap << "(true); }\n";
        accessors << "};\n";
    };

    for (const auto node_nested : file_schema.getProto().getNestedNodes()) {
        kj::StringPtr node_name = node_nested.getName();
        const auto& node = file_schema.getNested(node_name);
        kj::StringPtr proxied_class_type;
        if (auto proxy = GetAnnotation(node.getProto(), WRAP_ANNOTATION_ID)) {
            proxied_class_type = proxy->getText();
        }

        if (node.getProto().isStruct()) {
            const auto& struc = node.asStruct();
            std::ostringstream generic_name;
            generic_name << node_name;
            dec << "template<";
            bool first_param = true;
            for (const auto param : node.getProto().getParameters()) {
                if (first_param) {
                    first_param = false;
                    generic_name << "<";
                } else {
                    dec << ", ";
                    generic_name << ", ";
                }
                dec << "typename " << param.getName();
                generic_name << "" << param.getName();
            }
            if (!first_param) generic_name << ">";
            dec << ">\n";
            dec << "struct ProxyStruct<" << message_namespace << "::" << generic_name.str() << ">\n";
            dec << "{\n";
            dec << "    using Struct = " << message_namespace << "::" << generic_name.str() << ";\n";
            for (const auto field : struc.getFields()) {
                auto field_name = field.getProto().getName();
                add_accessor(field_name);
                dec << "    using " << Cap(field_name) << "Accessor = Accessor<" << output_name
                    << "_fields::" << Cap(field_name) << ", FIELD_IN | FIELD_OUT";
                if (BoxedType(field.getType())) dec << " | FIELD_BOXED";
                dec << ">;\n";
            }
            dec << "    using Accessors = std::tuple<";
            size_t i = 0;
            for (const auto field : struc.getFields()) {
                if (GetAnnotation(field.getProto(), SKIP_ANNOTATION_ID)) {
                    continue;
                }
                if (i) dec << ", ";
                dec << Cap(field.getProto().getName()) << "Accessor";
                ++i;
            }
            dec << ">;\n";
            dec << "    static constexpr size_t fields = " << i << ";\n";
            dec << "};\n";

            if (proxied_class_type.size()) {
                inl << "template<>\n";
                inl << "struct ProxyType<" << proxied_class_type << ">\n";
                inl << "{\n";
                inl << "public:\n";
                inl << "    using Struct = " << message_namespace << "::" << node_name << ";\n";
                size_t i = 0;
                for (const auto field : struc.getFields()) {
                    if (GetAnnotation(field.getProto(), SKIP_ANNOTATION_ID)) {
                        continue;
                    }
                    auto field_name = field.getProto().getName();
                    auto member_name = field_name;
                    if (auto name = GetAnnotation(field.getProto(), NAME_ANNOTATION_ID)) {
                        member_name = name->getText();
                    }
                    inl << "    static auto get(std::integral_constant<size_t, " << i << ">) -> AUTO_RETURN("
                        << "&" << proxied_class_type << "::" << member_name << ")\n";
                    ++i;
                }
                inl << "    static constexpr size_t fields = " << i << ";\n";
                inl << "};\n";
            }
        }

        if (proxied_class_type.size() && node.getProto().isInterface()) {
            const auto& interface = node.asInterface();

            std::ostringstream client;
            client << "template<>\nstruct ProxyClient<" << message_namespace << "::" << node_name << "> : ";
            client << "public ProxyClientCustom<" << message_namespace << "::" << node_name << ", "
                   << proxied_class_type << ">\n{\n";
            client << "public:\n";
            client << "    using ProxyClientCustom::ProxyClientCustom;\n";
            client << "    ~ProxyClient();\n";

            std::ostringstream server;
            server << "template<>\nstruct ProxyServer<" << message_namespace << "::" << node_name << "> : public "
                   << "ProxyServerCustom<" << message_namespace << "::" << node_name << ", " << proxied_class_type
                   << ">\n{\n";
            server << "public:\n";
            server << "    using ProxyServerCustom::ProxyServerCustom;\n";
            server << "    ~ProxyServer();\n";

            std::ostringstream client_construct;
            std::ostringstream client_destroy;

            for (const auto method : interface.getMethods()) {
                kj::StringPtr method_name = method.getProto().getName();
                kj::StringPtr proxied_method_name = method_name;
                if (auto name = GetAnnotation(method.getProto(), NAME_ANNOTATION_ID)) {
                    proxied_method_name = name->getText();
                }

                const std::string method_prefix = Format() << message_namespace << "::" << node_name
                                                           << "::" << Cap(method_name);
                bool is_construct = method_name == "construct";
                bool is_destroy = method_name == "destroy";

                struct Field
                {
                    boost::optional<::capnp::StructSchema::Field> param;
                    boost::optional<::capnp::StructSchema::Field> result;
                    int args = 0;
                    bool retval = false;
                    bool optional = false;
                    bool requested = false;
                    bool skip = false;
                    kj::StringPtr exception;
                };

                std::vector<Field> fields;
                std::map<kj::StringPtr, int> field_idx; // name -> args index
                bool has_result = false;

                auto add_field = [&](const ::capnp::StructSchema::Field& schema_field, bool param) {
                    if (GetAnnotation(schema_field.getProto(), SKIP_ANNOTATION_ID)) {
                        return;
                    }

                    auto field_name = schema_field.getProto().getName();
                    auto inserted = field_idx.emplace(field_name, fields.size());
                    if (inserted.second) {
                        fields.emplace_back();
                    }
                    auto& field = fields[inserted.first->second];
                    (param ? field.param : field.result) = schema_field;

                    if (!param && field_name == "result") {
                        field.retval = true;
                        has_result = true;
                    }

                    if (auto value = GetAnnotation(schema_field.getProto(), EXCEPTION_ANNOTATION_ID)) {
                        field.exception = value->getText();
                    }

                    boost::optional<int> count;
                    if (auto value = GetAnnotation(schema_field.getProto(), COUNT_ANNOTATION_ID)) {
                        count = value->getInt32();
                    } else if (schema_field.getType().isStruct()) {
                        if (auto value =
                                GetAnnotation(schema_field.getType().asStruct().getProto(), COUNT_ANNOTATION_ID)) {
                            count = value->getInt32();
                        }
                    } else if (schema_field.getType().isInterface()) {
                        if (auto value =
                                GetAnnotation(schema_field.getType().asInterface().getProto(), COUNT_ANNOTATION_ID)) {
                            count = value->getInt32();
                        }
                    }


                    if (inserted.second && !field.retval && !field.exception.size()) {
                        if (count) {
                            field.args = *count;
                        } else {
                            field.args = 1;
                        }
                    }
                };

                for (const auto schema_field : method.getParamType().getFields()) {
                    add_field(schema_field, true);
                }
                for (const auto schema_field : method.getResultType().getFields()) {
                    add_field(schema_field, false);
                }
                for (auto& field : field_idx) {
                    auto has_field = field_idx.find("has" + Cap(field.first));
                    if (has_field != field_idx.end()) {
                        fields[has_field->second].skip = true;
                        fields[field.second].optional = true;
                    }
                    auto want_field = field_idx.find("want" + Cap(field.first));
                    if (want_field != field_idx.end() && fields[want_field->second].param) {
                        fields[want_field->second].skip = true;
                        fields[field.second].requested = true;
                    }
                }

                if (!is_construct && !is_destroy) {
                    methods << "template<>\n";
                    methods << "struct ProxyMethod<" << method_prefix << "Params>\n";
                    methods << "{\n";
                    methods << "    static constexpr auto impl = &" << proxied_class_type
                            << "::" << proxied_method_name << ";\n";
                    methods << "};\n\n";
                }

                std::ostringstream client_args;
                std::ostringstream client_invoke;
                std::ostringstream server_invoke_start;
                std::ostringstream server_invoke_end;
                int argc = 0;
                for (const auto& field : fields) {
                    if (field.skip) continue;

                    auto field_name = field.param ? field.param->getProto().getName() :
                                                    field.result ? field.result->getProto().getName() : "";
                    auto field_type = field.param ? field.param->getType() : field.result->getType();

                    std::ostringstream field_flags;
                    field_flags << (!field.param ? "FIELD_OUT" : field.result ? "FIELD_IN | FIELD_OUT" : "FIELD_IN");
                    if (field.optional) field_flags << " | FIELD_OPTIONAL";
                    if (field.requested) field_flags << " | FIELD_REQUESTED";
                    if (BoxedType(field_type)) field_flags << " | FIELD_BOXED";

                    add_accessor(field_name);

                    for (int i = 0; i < field.args; ++i) {
                        if (argc > 0) client_args << ",";
                        client_args << "M" << method.getOrdinal() << "::Param<" << argc << "> " << field_name;
                        if (field.args > 1) client_args << i;
                        ++argc;
                    }
                    client_invoke << ", ";

                    if (field.exception.size()) {
                        client_invoke << "ClientException<" << field.exception << ", ";
                    } else {
                        client_invoke << "MakeClientParam<";
                    }

                    client_invoke << "Accessor<" << output_name << "_fields::" << Cap(field_name) << ", "
                                  << field_flags.str() << ">>(";

                    if (field.retval || field.args == 1) {
                        client_invoke << field_name;
                    } else {
                        for (int i = 0; i < field.args; ++i) {
                            if (i > 0) client_invoke << ", ";
                            client_invoke << field_name << i;
                        }
                    }
                    client_invoke << ")";

                    if (field.exception.size()) {
                        server_invoke_start << "Make<ServerExcept, " << field.exception;
                    } else if (field.retval) {
                        server_invoke_start << "Make<ServerRet";
                    } else {
                        server_invoke_start << "MakeServerField<" << field.args;
                    }
                    server_invoke_start << ", Accessor<" << output_name << "_fields::" << Cap(field_name) << ", "
                                        << field_flags.str() << ">>(";
                    server_invoke_end << ")";
                }

                client << "    using M" << method.getOrdinal() << " = ProxyClientMethodTraits<" << method_prefix
                       << "Params>;\n";
                client << "    typename M" << method.getOrdinal() << "::Result " << method_name << "("
                       << client_args.str() << ")";
                client << ";\n";
                def_client << "ProxyClient<" << message_namespace << "::" << node_name << ">::M" << method.getOrdinal()
                           << "::Result ProxyClient<" << message_namespace << "::" << node_name << ">::" << method_name
                           << "(" << client_args.str() << ") {\n";
                if (has_result) {
                    def_client << "    typename M" << method.getOrdinal() << "::Result result;\n";
                }
                def_client << "    clientInvoke(*this, &" << message_namespace << "::" << node_name
                           << "::Client::" << method_name << "Request" << client_invoke.str() << ");\n";
                if (has_result) def_client << "    return result;\n";
                def_client << "}\n";

                server << "    kj::Promise<void> " << method_name << "(" << Cap(method_name)
                       << "Context call_context) override;\n";

                def_server << "kj::Promise<void> ProxyServer<" << message_namespace << "::" << node_name
                           << ">::" << method_name << "(" << Cap(method_name)
                           << "Context call_context) {\n"
                              "    return serverInvoke(*this, call_context, "
                           << server_invoke_start.str();
                if (is_destroy) {
                    def_server << "ServerDestroy()";
                } else {
                    def_server << "ServerCall()";
                }
                def_server << server_invoke_end.str() << ");\n}\n";
            }

            client << "};\n";
            server << "};\n";
            dec << "\n" << client.str() << "\n" << server.str() << "\n";
            def_types << "ProxyClient<" << message_namespace << "::" << node_name
                      << ">::~ProxyClient() { clientDestroy(*this); " << client_destroy.str() << " }\n";
            def_types << "ProxyServer<" << message_namespace << "::" << node_name
                      << ">::~ProxyServer() { serverDestroy(*this); }\n";
        }
    }

    h << methods.str() << "namespace " << output_name << "_fields {\n"
      << accessors.str() << "} // namespace " << output_name << "_fields\n"
      << dec.str();

    cpp_server << def_server.str();
    cpp_server << "} // namespace mp\n";

    cpp_client << def_client.str();
    cpp_client << "} // namespace mp\n";

    cpp_types << def_types.str();
    cpp_types << "} // namespace mp\n";

    inl << "} // namespace mp\n";
    inl << "#endif\n";

    h << "} // namespace mp\n";
    h << "#endif\n";
}

int main(int argc, char** argv)
{
    if (argc < 3) {
        fprintf(stderr, "Usage: " PROXY_BIN " OUTPUT_STEM INPUT_SCHEMA [IMPORT_PATH...]\n");
        exit(1);
    }
    std::vector<kj::StringPtr> import_paths;
    for (size_t i = 3; i < argc; ++i) {
        import_paths.push_back(argv[i]);
    }
    Generate(argv[1], argv[2], {import_paths.data(), import_paths.size()});
    return 0;
}