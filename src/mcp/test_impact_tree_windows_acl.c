#include "mcp/test_impact_tree_internal.h"
#ifdef _WIN32
#include <aclapi.h>

bool tpt_win_effective(tpt_context *c) {
    cbm_pinned_tree_t *t = c->tree;
    if (t->impersonation != INVALID_HANDLE_VALUE)
        return tpt_fail(c, CBM_PINNED_TREE_UNSUPPORTED, "impersonated owner unavailable");
    if (OpenThreadToken(GetCurrentThread(), TOKEN_QUERY, TRUE, &t->impersonation))
        return tpt_fail(c, CBM_PINNED_TREE_UNSUPPORTED, "impersonated materializer unsupported");
    t->impersonation = INVALID_HANDLE_VALUE;
    return GetLastError() == ERROR_NO_TOKEN ||
           tpt_fail(c, CBM_PINNED_TREE_UNSUPPORTED, "effective identity inspection failed");
}

static bool tpt_token(tpt_context *c) {
    cbm_pinned_tree_t *t = c->tree;
    if (!tpt_win_effective(c))
        return false;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &t->token))
        return tpt_win_error(c, GetLastError(), false);
    DWORD needed = 0;
    if (GetTokenInformation(t->token, TokenUser, NULL, 0, &needed) ||
        GetLastError() != ERROR_INSUFFICIENT_BUFFER || needed < sizeof(TOKEN_USER) ||
        needed > TPT_SECURITY_CAP)
        return tpt_fail(c, CBM_PINNED_TREE_UNSUPPORTED, "unsupported process token representation");
    t->token_user = tpt_alloc(c, needed, 1);
    if (!t->token_user)
        return false;
    if (!GetTokenInformation(t->token, TokenUser, t->token_user, needed, &needed))
        return tpt_win_error(c, GetLastError(), false);
    PSID sid = ((TOKEN_USER *)t->token_user)->User.Sid;
    return IsValidSid(sid) || tpt_fail(c, CBM_PINNED_TREE_UNSUPPORTED, "invalid process user SID");
}

bool tpt_win_security(tpt_context *c) {
    cbm_pinned_tree_t *t = c->tree;
    if (!tpt_token(c))
        return false;
    PSID sid = ((TOKEN_USER *)t->token_user)->User.Sid;
    DWORD length = GetLengthSid(sid);
    if (length > SECURITY_MAX_SID_SIZE)
        return tpt_fail(c, CBM_PINNED_TREE_UNSUPPORTED, "unsupported user SID length");
    DWORD bytes = (DWORD)(sizeof(ACL) + sizeof(ACCESS_ALLOWED_ACE) - sizeof(DWORD)) + length;
    t->owned_acl = tpt_alloc(c, bytes, 1);
    if (!t->owned_acl)
        return false;
    if (!InitializeAcl(t->owned_acl, bytes, ACL_REVISION) ||
        !AddAccessAllowedAceEx(t->owned_acl, ACL_REVISION,
                               OBJECT_INHERIT_ACE | CONTAINER_INHERIT_ACE, FILE_ALL_ACCESS, sid) ||
        !InitializeSecurityDescriptor(&t->descriptor, SECURITY_DESCRIPTOR_REVISION) ||
        !SetSecurityDescriptorOwner(&t->descriptor, sid, FALSE) ||
        !SetSecurityDescriptorDacl(&t->descriptor, TRUE, t->owned_acl, FALSE) ||
        !SetSecurityDescriptorControl(&t->descriptor, SE_DACL_PROTECTED, SE_DACL_PROTECTED))
        return tpt_win_error(c, GetLastError(), false);
    t->attributes.nLength = sizeof(t->attributes);
    t->attributes.lpSecurityDescriptor = &t->descriptor;
    t->attributes.bInheritHandle = FALSE;
    return true;
}

static int tpt_owner_acl(cbm_pinned_tree_t *t, PSECURITY_DESCRIPTOR sd) {
    PSID owner = NULL;
    PACL dacl = NULL;
    BOOL owner_defaulted = FALSE, present = FALSE, defaulted = FALSE;
    SECURITY_DESCRIPTOR_CONTROL control = 0;
    DWORD revision = 0;
    if (!IsValidSecurityDescriptor(sd) ||
        !GetSecurityDescriptorOwner(sd, &owner, &owner_defaulted) ||
        !GetSecurityDescriptorDacl(sd, &present, &dacl, &defaulted) ||
        !GetSecurityDescriptorControl(sd, &control, &revision))
        return -1;
    PSID sid = ((TOKEN_USER *)t->token_user)->User.Sid;
    if (!owner || !IsValidSid(owner))
        return -1;
    if (!EqualSid(owner, sid) || !(control & SE_DACL_PROTECTED) || !present || !dacl)
        return 0;
    if (!IsValidAcl(dacl))
        return -1;
    ACL_SIZE_INFORMATION info;
    if (!GetAclInformation(dacl, &info, sizeof(info), AclSizeInformation))
        return -1;
    if (info.AceCount != 1)
        return 0;
    void *raw = NULL;
    if (!GetAce(dacl, 0, &raw))
        return -1;
    ACCESS_ALLOWED_ACE *ace = raw;
    if (ace->Header.AceType != ACCESS_ALLOWED_ACE_TYPE)
        return 0;
    size_t prefix = offsetof(ACCESS_ALLOWED_ACE, SidStart);
    if (ace->Header.AceSize < prefix + 8)
        return -1;
    if ((ace->Header.AceFlags & ~(OBJECT_INHERIT_ACE | CONTAINER_INHERIT_ACE)) ||
        (ace->Mask != FILE_ALL_ACCESS && ace->Mask != GENERIC_ALL))
        return 0;
    const SID *principal = (const SID *)&ace->SidStart;
    size_t sid_size = 8 + (size_t)principal->SubAuthorityCount * sizeof(DWORD);
    if (sid_size > ace->Header.AceSize - prefix || !IsValidSid(&ace->SidStart))
        return -1;
    return EqualSid(&ace->SidStart, sid) ? 1 : 0;
}

bool tpt_native_acl(tpt_context *c, tpt_object *o, unsigned mode, bool changed) {
    (void)mode;
    DWORD required = 0;
    if (!GetKernelObjectSecurity(o->handle, OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
                                 c->tree->security_scratch, TPT_SECURITY_CAP, &required)) {
        DWORD e = GetLastError();
        return tpt_fail(c,
                        e == ERROR_NOT_ENOUGH_MEMORY || e == ERROR_OUTOFMEMORY
                            ? CBM_PINNED_TREE_OOM
                            : CBM_PINNED_TREE_UNSUPPORTED,
                        "required owner/DACL inspection failed");
    }
    if (!required || required > TPT_SECURITY_CAP)
        return tpt_fail(c, CBM_PINNED_TREE_UNSUPPORTED, "unbounded native security descriptor");
    int allowed = tpt_owner_acl(c->tree, c->tree->security_scratch);
    if (allowed < 0)
        return tpt_fail(c, CBM_PINNED_TREE_UNSUPPORTED,
                        "native security descriptor cannot be decoded");
    return allowed == 1 ||
           tpt_fail(c, changed ? CBM_PINNED_TREE_CHANGED : CBM_PINNED_TREE_UNSUPPORTED,
                    "protected owner-only DACL policy mismatch");
}
#endif
