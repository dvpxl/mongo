/**
*    Copyright (C) 2013 10gen Inc.
*
*    This program is free software: you can redistribute it and/or  modify
*    it under the terms of the GNU Affero General Public License, version 3,
*    as published by the Free Software Foundation.
*
*    This program is distributed in the hope that it will be useful,
*    but WITHOUT ANY WARRANTY; without even the implied warranty of
*    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
*    GNU Affero General Public License for more details.
*
*    You should have received a copy of the GNU Affero General Public License
*    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*
*    As a special exception, the copyright holders give permission to link the
*    code of portions of this program with the OpenSSL library under certain
*    conditions as described in each individual source file and distribute
*    linked combinations including the program with the OpenSSL library. You
*    must comply with the GNU Affero General Public License in all respects for
*    all of the code used other than as permitted herein. If you modify file(s)
*    with this exception, you may extend this exception to your version of the
*    file(s), but you are not obligated to do so. If you do not wish to do so,
*    delete this exception statement from your version. If you delete this
*    exception statement from all source files in the program, then also delete
*    it in the license file.
*/

#include "mongo/db/auth/user_management_commands_parser.h"

#include <string>
#include <vector>

#include "mongo/base/status.h"
#include "mongo/bson/util/bson_extract.h"
#include "mongo/client/auth_helpers.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/auth/privilege_parser.h"
#include "mongo/db/auth/user_document_parser.h"
#include "mongo/db/auth/user_name.h"
#include "mongo/db/jsobj.h"
#include "mongo/platform/unordered_set.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {
namespace auth {

    /**
     * Writes into *writeConcern a BSONObj describing the parameters to getLastError to use for
     * the write confirmation.
     */
    Status _extractWriteConcern(const BSONObj& cmdObj, BSONObj* writeConcern) {
        BSONElement writeConcernElement;
        Status status = bsonExtractTypedField(cmdObj, "writeConcern", Object, &writeConcernElement);
        if (!status.isOK()) {
            if (status.code() == ErrorCodes::NoSuchKey) {
                *writeConcern = BSONObj();
                return Status::OK();
            }
            return status;
        }
        *writeConcern = writeConcernElement.Obj().getOwned();;
        return Status::OK();
    }

    Status _checkNoExtraFields(const BSONObj& cmdObj,
                              const StringData& cmdName,
                              const unordered_set<std::string>& validFieldNames) {
        // Iterate through all fields in command object and make sure there are no unexpected
        // ones.
        for (BSONObjIterator iter(cmdObj); iter.more(); iter.next()) {
            StringData fieldName = (*iter).fieldNameStringData();
            if (!validFieldNames.count(fieldName.toString())) {
                return Status(ErrorCodes::BadValue,
                              mongoutils::str::stream() << "\"" << fieldName << "\" is not "
                                      "a valid argument to " << cmdName);
            }
        }
        return Status::OK();
    }

    // Extracts a UserName or RoleName object from a BSONElement.
    template <typename Name>
    Status _parseNameFromBSONElement(const BSONElement& element,
                                     const StringData& dbname,
                                     const StringData& nameFieldName,
                                     const StringData& sourceFieldName,
                                     Name* parsedName) {
        if (element.type() == String) {
            *parsedName = Name(element.String(), dbname);
        }
        else if (element.type() == Object) {
            BSONObj obj = element.Obj();

            std::string name;
            std::string source;
            Status status = bsonExtractStringField(obj, nameFieldName, &name);
            if (!status.isOK()) {
                return status;
            }
            status = bsonExtractStringField(obj, sourceFieldName, &source);
            if (!status.isOK()) {
                return status;
            }

            *parsedName = Name(name, source);
        }
        else {
            return Status(ErrorCodes::BadValue,
                          "User and role names must be either strings or objects");
        }
        return Status::OK();
    }

    // Extracts UserName or RoleName objects from a BSONArray of role/user names.
    template <typename Name>
    Status _parseNamesFromBSONArray(const BSONArray& array,
                                    const StringData& dbname,
                                    const StringData& nameFieldName,
                                    const StringData& sourceFieldName,
                                    std::vector<Name>* parsedNames) {
        for (BSONObjIterator it(array); it.more(); it.next()) {
            BSONElement element = *it;
            Name name;
            Status status = _parseNameFromBSONElement(element,
                                                      dbname,
                                                      nameFieldName,
                                                      sourceFieldName,
                                                      &name);
            if (!status.isOK()) {
                return status;
            }
            parsedNames->push_back(name);
        }
        return Status::OK();
    }

    Status _parseUserNamesFromBSONArray(const BSONArray& usersArray,
                                        const StringData& dbname,
                                       std::vector<UserName>* parsedUserNames) {
        return _parseNamesFromBSONArray(usersArray,
                                        dbname,
                                        AuthorizationManager::USER_NAME_FIELD_NAME,
                                        AuthorizationManager::USER_SOURCE_FIELD_NAME,
                                        parsedUserNames);
    }

    Status parseRoleNamesFromBSONArray(const BSONArray& rolesArray,
                                       const StringData& dbname,
                                       std::vector<RoleName>* parsedRoleNames) {
        return _parseNamesFromBSONArray(rolesArray,
                                        dbname,
                                        AuthorizationManager::ROLE_NAME_FIELD_NAME,
                                        AuthorizationManager::ROLE_SOURCE_FIELD_NAME,
                                        parsedRoleNames);
    }

    Status parseRolePossessionManipulationCommands(const BSONObj& cmdObj,
                                                   const StringData& cmdName,
                                                   const StringData& rolesFieldName,
                                                   const std::string& dbname,
                                                   std::string* parsedName,
                                                   vector<RoleName>* parsedRoleNames,
                                                   BSONObj* parsedWriteConcern) {
        unordered_set<std::string> validFieldNames;
        validFieldNames.insert(cmdName.toString());
        validFieldNames.insert(rolesFieldName.toString());
        validFieldNames.insert("writeConcern");

        Status status = _checkNoExtraFields(cmdObj, cmdName, validFieldNames);
        if (!status.isOK()) {
            return status;
        }

        status = _extractWriteConcern(cmdObj, parsedWriteConcern);
        if (!status.isOK()) {
            return status;
        }

        status = bsonExtractStringField(cmdObj, cmdName, parsedName);
        if (!status.isOK()) {
            return status;
        }

        BSONElement rolesElement;
        status = bsonExtractTypedField(cmdObj, rolesFieldName, Array, &rolesElement);
        if (!status.isOK()) {
            return status;
        }

        status = parseRoleNamesFromBSONArray(BSONArray(rolesElement.Obj()),
                                             dbname,
                                             parsedRoleNames);
        if (!status.isOK()) {
            return status;
        }

        if (!parsedRoleNames->size()) {
            return Status(ErrorCodes::BadValue,
                          mongoutils::str::stream() << cmdName << " command requires a non-empty \""
                                  << rolesFieldName << "\" array");
        }
        return Status::OK();
    }

    Status parseCreateOrUpdateUserCommands(const BSONObj& cmdObj,
                                           const StringData& cmdName,
                                           const std::string& dbname,
                                           CreateOrUpdateUserArgs* parsedArgs) {
        unordered_set<std::string> validFieldNames;
        validFieldNames.insert(cmdName.toString());
        validFieldNames.insert("customData");
        validFieldNames.insert("pwd");
        validFieldNames.insert("roles");
        validFieldNames.insert("writeConcern");

        Status status = _checkNoExtraFields(cmdObj, cmdName, validFieldNames);
        if (!status.isOK()) {
            return status;
        }

        status = _extractWriteConcern(cmdObj, &parsedArgs->writeConcern);
        if (!status.isOK()) {
            return status;
        }

        BSONObjBuilder userObjBuilder;

        // Parse user name
        std::string userName;
        status = bsonExtractStringField(cmdObj, cmdName, &userName);
        if (!status.isOK()) {
            return status;
        }

        parsedArgs->userName = UserName(userName, dbname);

        // Parse password
        if (cmdObj.hasField("pwd")) {
            std::string clearTextPassword;
            status = bsonExtractStringField(cmdObj, "pwd", &clearTextPassword);
            if (!status.isOK()) {
                return status;
            }
            if (clearTextPassword.empty()) {
                return Status(ErrorCodes::BadValue, "User passwords must not be empty");
            }

            parsedArgs->hashedPassword = auth::createPasswordDigest(userName, clearTextPassword);
            parsedArgs->hasHashedPassword = true;
        }

        // Parse custom data
        if (cmdObj.hasField("customData")) {
            BSONElement element;
            status = bsonExtractTypedField(cmdObj, "customData", Object, &element);
            if (!status.isOK()) {
                return status;
            }
            parsedArgs->customData = element.Obj();
            parsedArgs->hasCustomData = true;
        }

        // Parse roles
        if (cmdObj.hasField("roles")) {
            BSONElement rolesElement;
            status = bsonExtractTypedField(cmdObj, "roles", Array, &rolesElement);
            if (!status.isOK()) {
                return status;
            }
            status = parseRoleNamesFromBSONArray(BSONArray(rolesElement.Obj()),
                                                 dbname,
                                                 &parsedArgs->roles);
            if (!status.isOK()) {
                return status;
            }
            parsedArgs->hasRoles = true;
        }

        return Status::OK();
    }

    Status parseAndValidateDropUserCommand(const BSONObj& cmdObj,
                                           const std::string& dbname,
                                           UserName* parsedUserName,
                                           BSONObj* parsedWriteConcern) {
        unordered_set<std::string> validFieldNames;
        validFieldNames.insert("dropUser");
        validFieldNames.insert("writeConcern");

        Status status = _checkNoExtraFields(cmdObj, "dropUser", validFieldNames);
        if (!status.isOK()) {
            return status;
        }

        std::string user;
        status = bsonExtractStringField(cmdObj, "dropUser", &user);
        if (!status.isOK()) {
            return status;
        }

        status = _extractWriteConcern(cmdObj, parsedWriteConcern);
        if (!status.isOK()) {
            return status;
        }

        *parsedUserName = UserName(user, dbname);
        return Status::OK();
    }

    Status parseAndValidateDropUsersFromDatabaseCommand(const BSONObj& cmdObj,
                                                        const std::string& dbname,
                                                        BSONObj* parsedWriteConcern) {
        unordered_set<std::string> validFieldNames;
        validFieldNames.insert("dropUsersFromDatabase");
        validFieldNames.insert("writeConcern");

        Status status = _checkNoExtraFields(cmdObj, "dropUsersFromDatabase", validFieldNames);
        if (!status.isOK()) {
            return status;
        }

        status = _extractWriteConcern(cmdObj, parsedWriteConcern);
        if (!status.isOK()) {
            return status;
        }

        return Status::OK();
    }

    Status parseUsersInfoCommand(const BSONObj& cmdObj,
                                 const StringData& dbname,
                                 UsersInfoArgs* parsedArgs) {
        unordered_set<std::string> validFieldNames;
        validFieldNames.insert("usersInfo");
        validFieldNames.insert("showPrivileges");
        validFieldNames.insert("showCredentials");

        Status status = _checkNoExtraFields(cmdObj, "usersInfo", validFieldNames);
        if (!status.isOK()) {
            return status;
        }

        if (cmdObj["usersInfo"].numberInt() == 1) {
            parsedArgs->allForDB = true;
        } else if (cmdObj["usersInfo"].type() == Array) {
            status = _parseUserNamesFromBSONArray(BSONArray(cmdObj["usersInfo"].Obj()),
                                                  dbname,
                                                  &parsedArgs->userNames);
            if (!status.isOK()) {
                return status;
            }
        } else {
            UserName name;
            status = _parseNameFromBSONElement(cmdObj["usersInfo"],
                                               dbname,
                                               AuthorizationManager::USER_NAME_FIELD_NAME,
                                               AuthorizationManager::USER_SOURCE_FIELD_NAME,
                                               &name);
            if (!status.isOK()) {
                return status;
            }
            parsedArgs->userNames.push_back(name);
        }

        status = bsonExtractBooleanFieldWithDefault(cmdObj,
                                                    "showPrivileges",
                                                    false,
                                                    &parsedArgs->showPrivileges);
        if (!status.isOK()) {
            return status;
        }
        status = bsonExtractBooleanFieldWithDefault(cmdObj,
                                                    "showCredentials",
                                                    false,
                                                    &parsedArgs->showCredentials);
        if (!status.isOK()) {
            return status;
        }

        return Status::OK();
    }

    Status parseRolesInfoCommand(const BSONObj& cmdObj,
                                 const StringData& dbname,
                                 std::vector<RoleName>* parsedRoleNames) {
        unordered_set<std::string> validFieldNames;
        validFieldNames.insert("rolesInfo");

        Status status = _checkNoExtraFields(cmdObj, "rolesInfo", validFieldNames);
        if (!status.isOK()) {
            return status;
        }

        if (cmdObj["rolesInfo"].type() == Array) {
            status = parseRoleNamesFromBSONArray(BSONArray(cmdObj["rolesInfo"].Obj()),
                                                 dbname,
                                                 parsedRoleNames);
            if (!status.isOK()) {
                return status;
            }
        } else {
            RoleName name;
            status = _parseNameFromBSONElement(cmdObj["rolesInfo"],
                                               dbname,
                                               AuthorizationManager::ROLE_NAME_FIELD_NAME,
                                               AuthorizationManager::ROLE_SOURCE_FIELD_NAME,
                                               &name);
            if (!status.isOK()) {
                return status;
            }
            parsedRoleNames->push_back(name);
        }

        return Status::OK();
    }

    /*
     * Validates that the given privilege BSONArray is valid.
     * If parsedPrivileges is not NULL, adds to it the privileges parsed out of the input BSONArray.
     */
    Status parseAndValidatePrivilegeArray(const BSONArray& privileges,
                                          PrivilegeVector* parsedPrivileges) {
        for (BSONObjIterator it(privileges); it.more(); it.next()) {
            BSONElement element = *it;
            if (element.type() != Object) {
                return Status(ErrorCodes::FailedToParse,
                              "Elements in privilege arrays must be objects");
            }

            ParsedPrivilege parsedPrivilege;
            std::string errmsg;
            if (!parsedPrivilege.parseBSON(element.Obj(), &errmsg)) {
                return Status(ErrorCodes::FailedToParse, errmsg);
            }
            if (!parsedPrivilege.isValid(&errmsg)) {
                return Status(ErrorCodes::FailedToParse, errmsg);
            }

            Privilege privilege;
            if (!ParsedPrivilege::parsedPrivilegeToPrivilege(parsedPrivilege, &privilege, &errmsg)) {
                return Status(ErrorCodes::FailedToParse, errmsg);
            }

            parsedPrivileges->push_back(privilege);
        }
        return Status::OK();
    }

    Status parseCreateOrUpdateRoleCommands(const BSONObj& cmdObj,
                                           const StringData& cmdName,
                                           const std::string& dbname,
                                           CreateOrUpdateRoleArgs* parsedArgs) {
        unordered_set<std::string> validFieldNames;
        validFieldNames.insert(cmdName.toString());
        validFieldNames.insert("privileges");
        validFieldNames.insert("roles");
        validFieldNames.insert("writeConcern");

        Status status = _checkNoExtraFields(cmdObj, cmdName, validFieldNames);
        if (!status.isOK()) {
            return status;
        }

        status = _extractWriteConcern(cmdObj, &parsedArgs->writeConcern);
        if (!status.isOK()) {
            return status;
        }

        std::string roleName;
        status = bsonExtractStringField(cmdObj, cmdName, &roleName);
        if (!status.isOK()) {
            return status;
        }
        parsedArgs->roleName = RoleName(roleName, dbname);

        // Parse privileges
        if (cmdObj.hasField("privileges")) {
            BSONElement privilegesElement;
            status = bsonExtractTypedField(cmdObj, "privileges", Array, &privilegesElement);
            if (!status.isOK()) {
                return status;
            }
            status = parseAndValidatePrivilegeArray(BSONArray(privilegesElement.Obj()),
                                                    &parsedArgs->privileges);
            if (!status.isOK()) {
                return status;
            }
            parsedArgs->hasPrivileges = true;
        }

        // Parse roles
        if (cmdObj.hasField("roles")) {
            BSONElement rolesElement;
            status = bsonExtractTypedField(cmdObj, "roles", Array, &rolesElement);
            if (!status.isOK()) {
                return status;
            }
            status = parseRoleNamesFromBSONArray(BSONArray(rolesElement.Obj()),
                                                 dbname,
                                                 &parsedArgs->roles);
            if (!status.isOK()) {
                return status;
            }
            parsedArgs->hasRoles = true;
        }
        return Status::OK();
    }

    Status parseAndValidateRolePrivilegeManipulationCommands(const BSONObj& cmdObj,
                                                             const StringData& cmdName,
                                                             const std::string& dbname,
                                                             RoleName* parsedRoleName,
                                                             PrivilegeVector* parsedPrivileges,
                                                             BSONObj* parsedWriteConcern) {
        unordered_set<std::string> validFieldNames;
        validFieldNames.insert(cmdName.toString());
        validFieldNames.insert("privileges");
        validFieldNames.insert("writeConcern");

        Status status = _checkNoExtraFields(cmdObj, cmdName, validFieldNames);
        if (!status.isOK()) {
            return status;
        }

        status = _extractWriteConcern(cmdObj, parsedWriteConcern);
        if (!status.isOK()) {
            return status;
        }

        BSONObjBuilder roleObjBuilder;

        // Parse role name
        std::string roleName;
        status = bsonExtractStringField(cmdObj, cmdName, &roleName);
        if (!status.isOK()) {
            return status;
        }
        *parsedRoleName = RoleName(roleName, dbname);

        // Parse privileges
        BSONElement privilegesElement;
        status = bsonExtractTypedField(cmdObj, "privileges", Array, &privilegesElement);
        if (!status.isOK()) {
            return status;
        }
        status = parseAndValidatePrivilegeArray(BSONArray(privilegesElement.Obj()),
                                                parsedPrivileges);
        if (!status.isOK()) {
            return status;
        }

        return Status::OK();
    }

    Status parseDropRoleCommand(const BSONObj& cmdObj,
                                const std::string& dbname,
                                RoleName* parsedRoleName,
                                BSONObj* parsedWriteConcern) {
        unordered_set<std::string> validFieldNames;
        validFieldNames.insert("dropRole");
        validFieldNames.insert("writeConcern");

        Status status = _checkNoExtraFields(cmdObj, "dropRole", validFieldNames);
        if (!status.isOK()) {
            return status;
        }

        std::string user;
        status = bsonExtractStringField(cmdObj, "dropRole", &user);
        if (!status.isOK()) {
            return status;
        }

        status = _extractWriteConcern(cmdObj, parsedWriteConcern);
        if (!status.isOK()) {
            return status;
        }

        *parsedRoleName = RoleName(user, dbname);
        return Status::OK();
    }

    Status parseDropRolesFromDatabaseCommand(const BSONObj& cmdObj,
                                             const std::string& dbname,
                                             BSONObj* parsedWriteConcern) {
        unordered_set<std::string> validFieldNames;
        validFieldNames.insert("dropRolesFromDatabase");
        validFieldNames.insert("writeConcern");

        Status status = _checkNoExtraFields(cmdObj, "dropRolesFromDatabase", validFieldNames);
        if (!status.isOK()) {
            return status;
        }

        status = _extractWriteConcern(cmdObj, parsedWriteConcern);
        if (!status.isOK()) {
            return status;
        }

        return Status::OK();
    }
} // namespace auth
} // namespace mongo
