/**
 * @file rootkeypackage_utils.c
 * @brief rootkeypackage_utils implementation.
 *
 * @copyright Copyright (c) Microsoft Corporation.
 * Licensed under the MIT License.
 */

#include "aduc/rootkeypackage_utils.h"
#include "aduc/rootkeypackage_parse.h"
#include <aduc/c_utils.h> // for EXTERN_C_BEGIN, EXTERN_C_END
#include <aduc/string_c_utils.h> // for IsNullOrEmpty
#include <azure_c_shared_utility/strings.h>
#include <azure_c_shared_utility/vector.h>
#include <parson.h>

EXTERN_C_BEGIN

/**
 * @brief Parses JSON string into an ADUC_RootKeyPackage struct.
 *
 * @param jsonString The root key package JSON string to parse.
 * @param outRootKeyPackage parameter for the resultant ADUC_RootKeyPackage.
 *
 * @return ADUC_Result The result of parsing.
 * @details Caller must call ADUC_RootKeyPackageUtils_Cleanup() on the resultant ADUC_RootKeyPackage.
 */
ADUC_Result ADUC_RootKeyPackageUtils_Parse(const char* jsonString, ADUC_RootKeyPackage* outRootKeyPackage)
{
    ADUC_Result result = { .ResultCode = ADUC_GeneralResult_Failure, .ExtendedResultCode = 0 };

    ADUC_RootKeyPackage pkg;
    memset(&pkg, 0, sizeof(pkg));

    JSON_Value* rootValue = NULL;
    JSON_Object* rootObj = NULL;

    if (IsNullOrEmpty(jsonString) || outRootKeyPackage == NULL)
    {
        result.ExtendedResultCode = ADUC_ERC_UTILITIES_ROOTKEYPKG_UTIL_ERROR_BAD_ARG;
        return result;
    }

    rootValue = json_parse_string(jsonString);
    if (rootValue == NULL)
    {
        result.ExtendedResultCode = ADUC_ERC_UTILITIES_ROOTKEYPKG_UTIL_ERROR_BAD_JSON;
        goto done;
    }

    rootObj = json_object(rootValue);
    if (rootObj == NULL)
    {
        result.ExtendedResultCode = ADUC_ERC_UTILITIES_ROOTKEYPKG_UTIL_ERROR_BAD_JSON;
        goto done;
    }

    result = RootKeyPackage_ParseProtectedProperties(rootObj, &pkg);
    if (IsAducResultCodeFailure(result.ResultCode))
    {
        goto done;
    }

    result = RootKeyPackage_ParseSignatures(rootObj, &pkg);
    if (IsAducResultCodeFailure(result.ResultCode))
    {
        goto done;
    }

    *outRootKeyPackage = pkg;
    memset(&pkg, 0, sizeof(pkg));

    result.ResultCode = ADUC_GeneralResult_Success;

done:
    json_value_free(rootValue);

    if (IsAducResultCodeFailure(result.ResultCode))
    {
        ADUC_RootKeyPackageUtils_Cleanup(&pkg);
    }

    return result;
}

/**
 * @brief Cleans up an ADUC_RootKeyPackage object.
 *
 * @param rootKeyPackage The root key package object to cleanup.
 */
void ADUC_RootKeyPackageUtils_Cleanup(ADUC_RootKeyPackage* rootKeyPackage)
{
    VECTOR_HANDLE vec = NULL;
    size_t cnt = 0;

    if (rootKeyPackage == NULL)
    {
        return;
    }

    //
    // Cleanup protectedProperties
    //

    // cleanup disabledRootKeys
    vec = rootKeyPackage->protectedProperties.disabledRootKeys;
    if (vec != NULL)
    {
        cnt = VECTOR_size(vec);
        for (size_t i = 0; i < cnt; ++i)
        {
            STRING_HANDLE* h = (STRING_HANDLE*)VECTOR_element(vec, i);
            if (h != NULL)
            {
                STRING_delete(*h);
            }
        }
        VECTOR_destroy(rootKeyPackage->protectedProperties.disabledRootKeys);
    }

    // cleanup disabledSigningKeys
    vec = rootKeyPackage->protectedProperties.disabledSigningKeys;
    if (vec != NULL)
    {
        cnt = VECTOR_size(vec);
        for (size_t i = 0; i < cnt; ++i)
        {
            ADUC_RootKeyPackage_Hash* node = (ADUC_RootKeyPackage_Hash*)VECTOR_element(vec, i);
            if (node != NULL && node->hash != NULL)
            {
                CONSTBUFFER_DecRef(node->hash);
            }
        }
        VECTOR_destroy(rootKeyPackage->protectedProperties.disabledSigningKeys);
    }

    // cleanup rootKeys
    vec = rootKeyPackage->protectedProperties.rootKeys;
    if (vec != NULL)
    {
        cnt = VECTOR_size(vec);
        for (size_t i = 0; i < cnt; ++i)
        {
            ADUC_RootKey* rootKeyEntry = (ADUC_RootKey*)VECTOR_element(vec, i);
            ADUC_RootKeyPackageUtils_RootKeyDeInit(rootKeyEntry);
        }
        VECTOR_destroy(rootKeyPackage->protectedProperties.rootKeys);
    }

    //
    // Cleanup signatures
    //
    vec = rootKeyPackage->signatures;
    if (vec != NULL)
    {
        cnt = VECTOR_size(vec);
        for (size_t i = 0; i < cnt; ++i)
        {
            ADUC_RootKeyPackage_Hash* node = (ADUC_RootKeyPackage_Hash*)VECTOR_element(vec, i);
            if (node != NULL && node->hash != NULL)
            {
                CONSTBUFFER_DecRef(node->hash);
            }
        }
        VECTOR_destroy(rootKeyPackage->signatures);
    }

    // finally, zero-out
    memset(rootKeyPackage, 0, sizeof(*rootKeyPackage));
}


_Bool ADUC_RootKeyPackageUtils_RootKey_Init(ADUC_RootKey** outKey, const char* kid, const ADUC_RootKey_KeyType keyType, const BUFFER_HANDLE n , const BUFFER_HANDLE e)
{
    _Bool success = false;
    if (outKey == NULL || n == NULL || e == NULL)
    {
        return false;
    }

    ADUC_RootKey* tempKey = (ADUC_RootKey*)malloc(sizeof(ADUC_RootKey));

    if (tempKey == NULL)
    {
        goto done;
    }

    STRING_HANDLE str_keyId = STRING_construct(kid);

    if (str_keyId == NULL)
    {
        goto done;
    }

    tempKey->kid = str_keyId;

    tempKey->keyType = keyType;

    tempKey->rsaParameters.e = CONSTBUFFER_CreateFromBuffer(e);
    tempKey->rsaParameters.n = CONSTBUFFER_CreateFromBuffer(n);

    success = true;

done:

    if (! success)
    {
        ADUC_RootKeyPackageUtils_RootKeyDeInit(tempKey);
        free(tempKey);

        tempKey = NULL;
    }

    *outKey = tempKey;

    return success;
}

void ADUC_RootKeyPackageUtils_RootKeyDeInit(ADUC_RootKey* key)
{
    if (key->kid != NULL)
    {
        STRING_delete(key->kid);
    }

    if (key->rsaParameters.n != NULL)
    {
        CONSTBUFFER_DecRef(key->rsaParameters.n);
    }

    if (key->rsaParameters.e != NULL)
    {
        CONSTBUFFER_DecRef(key->rsaParameters.e);
    }
}

EXTERN_C_END
