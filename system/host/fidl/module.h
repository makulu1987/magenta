// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#pragma once

#include <errno.h>

#include <map>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "ast.h"
#include "identifier_table.h"
#include "source_manager.h"
#include "type_shape.h"

namespace fidl {

template <typename T>
class Scope {
public:
    bool Insert(const T& t) {
        auto iter = scope_.insert(t);
        return iter.second;
    }

private:
    std::set<T> scope_;
};

// After name and type consumption of the AST, these types are no longer nested.
class Ordinal {
public:
    Ordinal(std::unique_ptr<NumericLiteral> literal, uint32_t value) :
        literal_(std::move(literal)),
        value_(value) {}

    uint32_t Value() const { return value_; }

private:
    std::unique_ptr<NumericLiteral> literal_;
    uint32_t value_;
};

class Name {
public:
    Name() : name_(nullptr) {}

    explicit Name(std::unique_ptr<Identifier> name) :
        name_(std::move(name)) {
    }

    StringView data() const {
        if (!name_)
            return StringView();
        return name_->identifier.data();
    }

private:
    std::unique_ptr<Identifier> name_;
};

struct ConstInfo {
    ConstInfo(Name name, std::unique_ptr<Type> type, std::unique_ptr<Constant> value) :
        name(std::move(name)),
        type(std::move(type)),
        value(std::move(value)) {}
    Name name;
    std::unique_ptr<Type> type;
    std::unique_ptr<Constant> value;
};

struct EnumInfo {
    struct Member {
        Member(Name name, std::unique_ptr<Constant> value) :
            name(std::move(name)),
            value(std::move(value)) {}
        Name name;
        std::unique_ptr<Constant> value;
    };

    EnumInfo(Name name, std::unique_ptr<PrimitiveType> type, std::vector<Member> members) :
        name(std::move(name)),
        type(std::move(type)),
        members(std::move(members)) {}

    Name name;
    std::unique_ptr<PrimitiveType> type;
    std::vector<Member> members;
};

struct InterfaceInfo {
    struct Method {
        struct Parameter {
            Parameter(std::unique_ptr<Type> type, Name name) :
                type(std::move(type)),
                name(std::move(name)) {}
            std::unique_ptr<Type> type;
            Name name;
        };

        Method(Method&&) = default;
        Method& operator=(Method&&) = default;

        Method(Ordinal ordinal, Name name, std::vector<Parameter> parameter_list,
               bool has_response, std::vector<Parameter> maybe_response) :
            ordinal(std::move(ordinal)),
            name(std::move(name)),
            parameter_list(std::move(parameter_list)),
            has_response(has_response),
            maybe_response(std::move(maybe_response)) {}

        Ordinal ordinal;
        Name name;
        std::vector<Parameter> parameter_list;
        bool has_response;
        std::vector<Parameter> maybe_response;
    };

    InterfaceInfo(Name name, std::vector<Method> methods) :
        name(std::move(name)),
        methods(std::move(methods)) {}

    Name name;
    std::vector<Method> methods;
};

struct StructInfo {
    struct Member {
        Member(std::unique_ptr<Type> type, Name name, std::unique_ptr<Constant> default_value) :
            type(std::move(type)),
            name(std::move(name)),
            default_value(std::move(default_value)) {}
        std::unique_ptr<Type> type;
        Name name;
        std::unique_ptr<Constant> default_value;
    };

    StructInfo(Name name, std::vector<Member> members) :
        name(std::move(name)),
        members(std::move(members)) {}

    Name name;
    std::vector<Member> members;
};

struct UnionInfo {
    struct Member {
        Member(std::unique_ptr<Type> type, Name name) :
            type(std::move(type)),
            name(std::move(name)) {}
        std::unique_ptr<Type> type;
        Name name;
    };

    UnionInfo(Name name, std::vector<Member> members) :
        name(std::move(name)),
        members(std::move(members)) {}

    Name name;
    std::vector<Member> members;
};

class Module {
public:
    bool CreateSource(const char* file_name, StringView* source_out) {
        return source_manager_.CreateSource(file_name, source_out);
    }
    bool Parse(StringView source);
    bool Dump();

private:
    bool ConsumeConstDeclaration(std::unique_ptr<ConstDeclaration> const_declaration);
    bool ConsumeEnumDeclaration(std::unique_ptr<EnumDeclaration> enum_declaration);
    bool ConsumeInterfaceDeclaration(std::unique_ptr<InterfaceDeclaration> interface_declaration);
    bool ConsumeStructDeclaration(std::unique_ptr<StructDeclaration> struct_declaration);
    bool ConsumeUnionDeclaration(std::unique_ptr<UnionDeclaration> union_declaration);
    bool ConsumeFile(std::unique_ptr<File> file);

    bool RegisterTypeName(const Name& name);

    bool ResolveConstInfo(const ConstInfo& const_info);
    bool ResolveEnumInfo(const EnumInfo& enum_info);
    bool ResolveInterfaceInfo(const InterfaceInfo& interface_info);
    bool ResolveStructInfo(const StructInfo& struct_info);
    bool ResolveUnionInfo(const UnionInfo& union_info);
    bool Resolve();

    bool ResolveArrayType(const ArrayType& array_type, TypeShape* out_type_metadata);
    bool ResolveVectorType(const VectorType& vector_type, TypeShape* out_type_metadata);
    bool ResolveStringType(const StringType& string_type, TypeShape* out_type_metadata);
    bool ResolveHandleType(const HandleType& handle_type, TypeShape* out_type_metadata);
    bool ResolveRequestType(const RequestType& request_type, TypeShape* out_type_metadata);
    bool ResolvePrimitiveType(const PrimitiveType& primitive_type, TypeShape* out_type_metadata);
    bool ResolveIdentifierType(const IdentifierType& identifier_type, TypeShape* out_type_metadata);
    bool ResolveType(const Type* type) {
        TypeShape type_metadata;
        return ResolveType(type, &type_metadata);
    }
    bool ResolveType(const Type* type, TypeShape* out_type_metadata);
    bool ResolveTypeName(const CompoundIdentifier* name);
    bool RegisterResolvedType(const Name& name, TypeShape type_metadata);

    std::vector<ConstInfo> const_infos_;
    std::vector<EnumInfo> enum_infos_;
    std::vector<InterfaceInfo> interface_infos_;
    std::vector<StructInfo> struct_infos_;
    std::vector<UnionInfo> union_infos_;

    std::set<StringView> registered_types_;
    std::map<StringView, TypeShape> resolved_types_;

    std::vector<std::set<StringView>> scoped_names_;

    IdentifierTable identifier_table_;
    SourceManager source_manager_;
};

} // namespace fidl
