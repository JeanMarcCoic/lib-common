/***************************************************************************/
/*                                                                         */
/* Copyright 2019 INTERSEC SA                                              */
/*                                                                         */
/* Licensed under the Apache License, Version 2.0 (the "License");         */
/* you may not use this file except in compliance with the License.        */
/* You may obtain a copy of the License at                                 */
/*                                                                         */
/*     http://www.apache.org/licenses/LICENSE-2.0                          */
/*                                                                         */
/* Unless required by applicable law or agreed to in writing, software     */
/* distributed under the License is distributed on an "AS IS" BASIS,       */
/* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.*/
/* See the License for the specific language governing permissions and     */
/* limitations under the License.                                          */
/*                                                                         */
/***************************************************************************/

#include <math.h>

#include <lib-common/unix.h>
#include <lib-common/yaml.h>
#include <lib-common/iop-yaml.h>
#include <lib-common/log.h>

#include "helpers.in.c"

static struct iop_yaml_g {
    logger_t logger;
} iop_yaml_g = {
#define _G iop_yaml_g
    .logger = LOGGER_INIT(NULL, "iop-yaml", LOG_INHERITS),
};

/* {{{ yunpack */

typedef struct yunpack_error_t {
    /* the yaml data that caused the error */
    const yaml_data_t * nonnull data;
    /* details of the error */
    sb_t buf;
} yunpack_error_t;

typedef struct yunpack_env_t {
    mem_pool_t *mp;

    yunpack_error_t err;

    /* Only IOP_UNPACK_FORBID_PRIVATE is handled. */
    int flags;
} yunpack_env_t;

/* {{{ Yaml scalar to iop field */

/* FIXME: compare with JSON to have as few type mismatch as possible, for
 * backward-compatibility */

typedef enum yunpack_res_t {
    YUNPACK_NULL_STRUCT_ERROR = -5,
    YUNPACK_INVALID_B64_VAL = -4,
    YUNPACK_INVALID_ENUM_VAL = -3,
    YUNPACK_TYPE_MISMATCH = -2,
    YUNPACK_OOB = -1,
    YUNPACK_OK = 0,
} yunpack_res_t;

static int
yaml_data_to_typed_struct(yunpack_env_t * nonnull env,
                          const yaml_data_t * nonnull data,
                          const iop_struct_t * nonnull st,
                          void * nonnull out);

static yunpack_res_t
yaml_nil_to_iop_field(yunpack_env_t * nonnull env,
                      const yaml_data_t * nonnull data,
                      const iop_field_t * nonnull fdesc,
                      bool in_array, void * nonnull out)
{
    if (!in_array && fdesc->repeat == IOP_R_REPEATED) {
        lstr_t *arr = out;

        /* null value on an array means empty array */
        p_clear(arr, 1);
        return YUNPACK_OK;
    }

    switch (fdesc->type) {
      case IOP_T_STRING:
      case IOP_T_XML:
      case IOP_T_DATA:
        *(lstr_t *)out = LSTR_NULL_V;
        return YUNPACK_OK;
      case IOP_T_VOID:
        return YUNPACK_OK;
      case IOP_T_STRUCT:
      case IOP_T_UNION:
        if (yaml_data_to_typed_struct(env, data, fdesc->u1.st_desc,
                                      out) < 0)
        {
            return YUNPACK_NULL_STRUCT_ERROR;
        }
        return YUNPACK_OK;
      default:
        return YUNPACK_TYPE_MISMATCH;
    }
}

static yunpack_res_t
yaml_string_to_iop_field(mem_pool_t *mp, const lstr_t str,
                         const iop_field_t * nonnull fdesc,
                         void * nonnull out)
{
    switch (fdesc->type) {
      case IOP_T_STRING:
      case IOP_T_XML:
        *(lstr_t *)out = mp_lstr_dup(mp, str);
        return YUNPACK_OK;

      case IOP_T_DATA: {
        sb_t  sb;
        /* TODO: factorize this with iop-json, iop-xml, etc */
        int   blen = DIV_ROUND_UP(str.len * 3, 4);
        char *buf  = mp_new_raw(mp, char, blen + 1);
        lstr_t *data = out;

        sb_init_full(&sb, buf, 0, blen + 1, &mem_pool_static);
        if (sb_add_lstr_unb64(&sb, str) < 0) {
            mp_delete(mp, &buf);
            return YUNPACK_INVALID_B64_VAL;
        }
        data->data = buf;
        data->len  = sb.len;
        return YUNPACK_OK;
      }

      case IOP_T_ENUM: {
        bool found;
        int i;

        i = iop_enum_from_lstr_desc(fdesc->u1.en_desc, str, &found);
        if (!found) {
            return YUNPACK_INVALID_ENUM_VAL;
        }
        *(int32_t *)out = i;
        return 0;
      }

      default:
        return YUNPACK_TYPE_MISMATCH;
    }
}

static void
set_string_from_stream(mem_pool_t * nonnull mp,
                       const yaml_data_t * nonnull data,
                       lstr_t * nonnull out)
{
    *out = mp_lstr_dups(mp, data->pos_start.s,
                        data->pos_end.s - data->pos_start.s);
}

static yunpack_res_t
yaml_double_to_iop_field(mem_pool_t * nonnull mp,
                         const yaml_data_t * nonnull data,
                         double d, const iop_field_t * nonnull fdesc,
                         void * nonnull out)
{
    switch (fdesc->type) {
      case IOP_T_DOUBLE:
        *(double *)out = d;
        return YUNPACK_OK;
      case IOP_T_STRING:
        set_string_from_stream(mp, data, out);
        return YUNPACK_OK;
      default:
        return YUNPACK_TYPE_MISMATCH;
    }
}

static int
yaml_uint_to_iop_field(mem_pool_t * nonnull mp,
                       const yaml_data_t * nonnull data,
                       uint64_t u, const iop_field_t * nonnull fdesc,
                       void * nonnull out)
{
#define CHECK_MAX(v, max)  THROW_IF(v > max, YUNPACK_OOB)

    switch (fdesc->type) {
      case IOP_T_I8:
        CHECK_MAX(u, INT8_MAX);
        *(int8_t *)out = u;
        return YUNPACK_OK;
      case IOP_T_U8:
        CHECK_MAX(u, UINT8_MAX);
        *(uint8_t *)out = u;
        return YUNPACK_OK;
      case IOP_T_I16:
        CHECK_MAX(u, INT16_MAX);
        *(int16_t *)out = u;
        return YUNPACK_OK;
      case IOP_T_U16:
        CHECK_MAX(u, UINT16_MAX);
        *(uint16_t *)out = u;
        return YUNPACK_OK;
      case IOP_T_I32:
        CHECK_MAX(u, INT32_MAX);
        *(int32_t *)out = u;
        return YUNPACK_OK;
      case IOP_T_U32:
        CHECK_MAX(u, UINT32_MAX);
        *(uint32_t *)out = u;
        return YUNPACK_OK;
      case IOP_T_I64:
        CHECK_MAX(u, INT64_MAX);
        *(int64_t *)out = u;
        return YUNPACK_OK;
      case IOP_T_U64:
        *(uint64_t *)out = u;
        return YUNPACK_OK;
      case IOP_T_STRING:
        set_string_from_stream(mp, data, out);
        return YUNPACK_OK;
      case IOP_T_ENUM:
        CHECK_MAX(u, INT32_MAX);
        if (TST_BIT(&fdesc->u1.en_desc->flags, IOP_ENUM_STRICT)
        &&  !iop_enum_exists_desc(fdesc->u1.en_desc, u))
        {
            return YUNPACK_INVALID_ENUM_VAL;
        }
        *(int32_t *)out = u;
        return YUNPACK_OK;
      default:
        return YUNPACK_TYPE_MISMATCH;
    }

#undef CHECK_MAX
}

static int
yaml_int_to_iop_field(int64_t i, const iop_field_t * nonnull fdesc,
                      void * nonnull out)
{
#define CHECK_RANGE(v, min, max)  THROW_IF(v < min || v > max, YUNPACK_OOB)

    /* The INT type is only used for negative values, so all unsigned types
     * are OOB. */

    switch (fdesc->type) {
      case IOP_T_I8:
        CHECK_RANGE(i, INT8_MIN, INT8_MAX);
        *(int8_t *)out = i;
        return YUNPACK_OK;
      case IOP_T_U8:
        return YUNPACK_OOB;
      case IOP_T_I16:
        CHECK_RANGE(i, INT16_MIN, INT16_MAX);
        *(int16_t *)out = i;
        return YUNPACK_OK;
      case IOP_T_U16:
        return YUNPACK_OOB;
      case IOP_T_I32:
        CHECK_RANGE(i, INT32_MIN, INT32_MAX);
        *(int32_t *)out = i;
        return YUNPACK_OK;
      case IOP_T_U32:
        return YUNPACK_OOB;
      case IOP_T_I64:
        *(int64_t *)out = i;
        return YUNPACK_OK;
      case IOP_T_U64:
        return YUNPACK_OOB;
      case IOP_T_ENUM:
        CHECK_RANGE(i, INT32_MIN, INT32_MAX);
        if (TST_BIT(&fdesc->u1.en_desc->flags, IOP_ENUM_STRICT)
        &&  !iop_enum_exists_desc(fdesc->u1.en_desc, i))
        {
            return YUNPACK_INVALID_ENUM_VAL;
        }
        *(int32_t *)out = i;
        return YUNPACK_OK;
      default:
        return YUNPACK_TYPE_MISMATCH;
    }
}

#undef CHECK_RANGE

static int
yaml_bool_to_iop_field(mem_pool_t * nonnull mp,
                       const yaml_data_t * nonnull data,
                       bool b, const iop_field_t * nonnull fdesc,
                       void * nonnull out)
{
    switch (fdesc->type) {
      case IOP_T_BOOL:
        *(bool *)out = b;
        return YUNPACK_OK;
      case IOP_T_STRING:
        set_string_from_stream(mp, data, out);
        return YUNPACK_OK;
      default:
        return YUNPACK_TYPE_MISMATCH;
    }
}

static yunpack_res_t
yaml_scalar_to_iop_field(yunpack_env_t * nonnull env,
                         const yaml_data_t * nonnull data,
                         const iop_field_t * nonnull fdesc,
                         bool in_array, void * nonnull out)
{
    const yaml_scalar_t *scalar = &data->scalar;

    /* backward compatibility: unpack a scalar into an array as an array
     * of one element. */
    if (!in_array && fdesc->repeat == IOP_R_REPEATED
    &&  scalar->type != YAML_SCALAR_NULL)
    {
        lstr_t *arr = out;

        out = mp_imalloc(env->mp, fdesc->size, 8, 0);
        *arr = mp_lstr_init(env->mp, out, 1);
    }

    switch (scalar->type) {
      case YAML_SCALAR_NULL:
        return yaml_nil_to_iop_field(env, data, fdesc, in_array, out);
      case YAML_SCALAR_STRING:
        return yaml_string_to_iop_field(env->mp, scalar->s, fdesc, out);
      case YAML_SCALAR_DOUBLE:
        return yaml_double_to_iop_field(env->mp, data, scalar->d, fdesc, out);
      case YAML_SCALAR_UINT:
        return yaml_uint_to_iop_field(env->mp, data, scalar->u, fdesc, out);
      case YAML_SCALAR_INT:
        return yaml_int_to_iop_field(scalar->i, fdesc, out);
      case YAML_SCALAR_BOOL:
        return yaml_bool_to_iop_field(env->mp, data, scalar->b, fdesc, out);
    }

    assert (false);
    return YUNPACK_TYPE_MISMATCH;
}

/* }}} */
/* {{{ Yaml data to union */

/* XXX: the in_array argument is required for the "scalar -> [scalar]"
 * backward compat. When unpacking a sequence, it is false, but when true,
 * arrays are detected and scalars are automatically put in a single element
 * array. */
static int
yaml_data_to_iop_field(yunpack_env_t * nonnull env,
                       const yaml_data_t * nonnull data,
                       const iop_struct_t * nonnull st_desc,
                       const iop_field_t * nonnull fdesc,
                       bool in_array, void * nonnull out);

static int
check_constraints(const iop_struct_t * nonnull desc,
                  const iop_field_t * nonnull fdesc, void * nonnull value)
{
    if (likely(!iop_field_has_constraints(desc, fdesc))) {
        return 0;
    }

    if (fdesc->repeat == IOP_R_REPEATED) {
        iop_array_i8_t *arr = value;

        return iop_field_check_constraints(desc, fdesc, arr->tab, arr->len,
                                           false);
    } else {
        return iop_field_check_constraints(desc, fdesc, value, 1, false);
    }
}

static int
yaml_data_to_union(yunpack_env_t * nonnull env,
                   const yaml_data_t * nonnull data,
                   const iop_struct_t * nonnull st_desc, void * nonnull out)
{
    const iop_field_t *field_desc = NULL;
    const yaml_key_data_t *kd;

    if (data->type != YAML_DATA_OBJ) {
        sb_setf(&env->err.buf, "cannot unpack %s into a union",
                yaml_data_get_type(data, false));
        goto error;
    }

    if (data->obj->fields.len != 1) {
        sb_setf(&env->err.buf, "a single key must be specified");
        goto error;
    }
    kd = &data->obj->fields.tab[0];

    iop_field_find_by_name(st_desc, kd->key, NULL, &field_desc);
    if (!field_desc) {
        sb_setf(&env->err.buf, "unknown field `%pL`", &kd->key);
        goto error;
    }

    iop_union_set_tag(st_desc, field_desc->tag, out);
    out = (char *)out + field_desc->data_offs;
    if (yaml_data_to_iop_field(env, &kd->data, st_desc, field_desc, false,
                               out) < 0)
    {
        /* keep the data causing the issue in the err. */
        data = env->err.data;
        goto error;
    }

    if (check_constraints(st_desc, field_desc, out) < 0) {
        sb_setf(&env->err.buf, "field `%pL` is invalid: %s", &kd->key,
                iop_get_err());
        goto error;
    }

    return 0;

  error:
    sb_prependf(&env->err.buf, "cannot unpack YAML as a `%pL` IOP union: ",
                &st_desc->fullname);
    env->err.data = data;
    return -1;
}

/* }}} */
/* {{{ Yaml obj to iop field */

static int
check_class(yunpack_env_t * nonnull env,
            const iop_struct_t * nonnull st)
{
    if (st->class_attrs->is_abstract) {
        sb_setf(&env->err.buf, "`%pL` is abstract and cannot be unpacked",
                &st->fullname);
        return -1;
    }

    if (env->flags & IOP_UNPACK_FORBID_PRIVATE
    &&  st->class_attrs->is_private)
    {
        /* TODO: the error should probably be an "unknown type" to not expose
         * the private name. To be done for JSON unpacker too. */
        sb_setf(&env->err.buf, "`%pL` is private and cannot be unpacked",
                &st->fullname);
        return -1;
    }

    return 0;
}

static const yaml_data_t * nullable
yaml_data_get_field_value(const yaml_data_t * nonnull data,
                          const lstr_t field_name)
{
    if (data->type == YAML_DATA_OBJ) {
        /* do a linear search in the obj AST. This isn't necessarily optimal,
         * but keeps the code simple. */
        tab_for_each_ptr(pair, &data->obj->fields) {
            if (lstr_equal(pair->key, field_name)) {
                return &pair->data;
            }
        }
        return NULL;
    } else {
        assert (data->type == YAML_DATA_SCALAR
            &&  data->scalar.type == YAML_SCALAR_NULL);
        return NULL;
    }
}

static int
yaml_skip_iop_field(yunpack_env_t * nonnull env,
                    const iop_struct_t * nonnull st,
                    const iop_field_t * nonnull fdesc, void * nonnull out)
{
    if (iop_skip_absent_field_desc(env->mp, out, st, fdesc) < 0) {
        const char *iop_err = iop_get_err();

        if (iop_err) {
            sb_setf(&env->err.buf, "field `%pL` is invalid: %s",
                    &fdesc->name, iop_err);
        } else {
            sb_setf(&env->err.buf, "missing field `%pL`", &fdesc->name);
        }
        return -1;
    }
    return 0;
}

static int
yaml_fill_iop_field(yunpack_env_t * nonnull env,
                    const yaml_data_t * nonnull data,
                    const iop_struct_t * nonnull st,
                    const iop_field_t * nonnull fdesc, void * nonnull out)
{
    if (env->flags & IOP_UNPACK_FORBID_PRIVATE) {
        const iop_field_attrs_t *attrs;

        attrs = iop_field_get_attrs(st, fdesc);
        if (attrs && TST_BIT(&attrs->flags, IOP_FIELD_PRIVATE)) {
            sb_setf(&env->err.buf, "unknown field `%pL`", &fdesc->name);
            return -1;
        }
    }

    out = (char *)out + fdesc->data_offs;
    if (yaml_data_to_iop_field(env, data, st, fdesc, false, out) < 0) {
        return -1;
    }

    if (check_constraints(st, fdesc, out) < 0) {
        sb_setf(&env->err.buf, "field `%pL` is invalid: %s", &fdesc->name,
                iop_get_err());
        return -1;
    }

    return 0;
}

static void
yaml_data_find_extra_key(yunpack_env_t * nonnull env,
                         const yaml_data_t * nonnull data,
                         const iop_struct_t * nonnull st)
{
    tab_for_each_ptr(pair, &data->obj->fields) {
        if (iop_field_find_by_name(st, pair->key, NULL, NULL) < 0) {
            sb_setf(&env->err.buf, "unknown field `%pL`", &pair->key);
            return;
        }
    }

    assert (false);
}

static int
yaml_data_to_typed_struct(yunpack_env_t * nonnull env,
                          const yaml_data_t * nonnull data,
                          const iop_struct_t * nonnull st,
                          void * nonnull out)
{
    const iop_struct_t *real_st = st;
    int nb_fields_matched;

    switch (data->type) {
      case YAML_DATA_SCALAR:
        if (data->scalar.type == YAML_SCALAR_NULL) {
            break;
        }
        /* FALLTHROUGH */
      case YAML_DATA_SEQ:
        sb_setf(&env->err.buf, "cannot unpack %s into a %s",
                yaml_data_get_type(data, false),
                real_st->is_union ? "union" : "struct");
        goto error;
      case YAML_DATA_OBJ:
        break;
    }

    if (data->tag.s) {
        if (iop_struct_is_class(st)) {
            real_st = iop_get_class_by_fullname(st, data->tag);
            if (!real_st) {
                sb_setf(&env->err.buf, "unknown type `%pL` provided in tag, "
                        "or not a child of `%pL`", &data->tag,
                        &st->fullname);
                real_st = st;
                goto error;
            }
            if (!iop_class_is_a(real_st, st)) {
                sb_setf(&env->err.buf, "provided tag `%pL` is not a child of "
                        "`%pL`", &real_st->fullname, &st->fullname);
                real_st = st;
                goto error;
            }
        } else {
            if (!lstr_equal(st->fullname, data->tag)) {
                sb_setf(&env->err.buf, "wrong type `%pL` provided in tag, "
                        "expected `%pL`", &data->tag, &st->fullname);
                goto error;
            }
        }
    }

    if (st->is_union) {
        return yaml_data_to_union(env, data, st, out);
    }

    if (iop_struct_is_class(real_st)) {
        void **out_class = out;

        if (check_class(env, real_st) < 0) {
            goto error;
        }

        *out_class = mp_imalloc(env->mp, real_st->size, 8, MEM_RAW);
        *(const iop_struct_t **)(*out_class) = real_st;
        out = *out_class;
    }

    st = real_st;
    nb_fields_matched = 0;
    iop_struct_for_each_field(field_desc, field_st, real_st) {
        const yaml_data_t *val;

        val = yaml_data_get_field_value(data, field_desc->name);
        if (val) {
            if (yaml_fill_iop_field(env, val, field_st, field_desc, out) < 0)
            {
                goto error;
            }
            nb_fields_matched++;
        } else {
            if (yaml_skip_iop_field(env, field_st, field_desc, out) < 0) {
                goto error;
            }
        }
    }

    if (data->type == YAML_DATA_OBJ
    &&  unlikely(nb_fields_matched != data->obj->fields.len))
    {
        /* There are fields in the YAML object that have not been matched.
         * The handling of this error is kept in a cold path, as it is
         * supposed to be rare. */
        assert (nb_fields_matched < data->obj->fields.len);
        yaml_data_find_extra_key(env, data, real_st);
        goto error;
    }

    return 0;

  error:
    sb_prependf(&env->err.buf, "cannot unpack YAML as a `%pL` IOP %s: ",
                &real_st->fullname, real_st->is_union ? "union" : "struct");
    if (!env->err.data) {
        env->err.data = data;
    }
    return -1;
}

static void yaml_set_type_mismatch_err(yunpack_env_t * nonnull env,
                                       const yaml_data_t * nonnull data,
                                       const iop_field_t * nonnull fdesc)
{
    sb_setf(&env->err.buf, "cannot set %s in a field of type %s",
            yaml_data_get_type(data, false),
            iop_type_get_string_desc(fdesc->type));
    env->err.data = data;
}

/* }}} */
/* {{{ Yaml seq to iop field */

static int
yaml_seq_to_iop_field(yunpack_env_t * nonnull env,
                      const yaml_data_t * nonnull data,
                      const iop_struct_t * nonnull st_desc,
                      const iop_field_t * nonnull fdesc, void * nonnull out)
{
    lstr_t *arr = out;
    int size = 0;

    if (fdesc->repeat != IOP_R_REPEATED) {
        sb_sets(&env->err.buf, "cannot set a sequence in a non-array field");
        env->err.data = data;
        return -1;
    }

    p_clear(arr, 1);
    assert (data->type == YAML_DATA_SEQ);
    tab_for_each_ptr(elem, &data->seq->datas) {
        void *elem_out;

        if (arr->len >= size) {
            size = p_alloc_nr(size);
            arr->data = mp_irealloc(env->mp, arr->data,
                                    arr->len * fdesc->size,
                                    size * fdesc->size, 8, 0);
        }
        elem_out = (void *)(((char *)arr->data) + arr->len * fdesc->size);
        RETHROW(yaml_data_to_iop_field(env, elem, st_desc, fdesc, true,
                                       elem_out));
        arr->len++;
    }

    return 0;
}

/* }}} */
/* {{{ Yaml data to iop field */

static int
yaml_data_to_iop_field(yunpack_env_t *env, const yaml_data_t * nonnull data,
                       const iop_struct_t * nonnull st_desc,
                       const iop_field_t * nonnull fdesc,
                       bool in_array, void * nonnull out)
{
    bool struct_or_union;

    struct_or_union = fdesc->type == IOP_T_STRUCT
                   || fdesc->type == IOP_T_UNION;
    if (struct_or_union && iop_field_is_reference(fdesc)) {
        out = iop_field_ptr_alloc(env->mp, fdesc, out);
    } else
    if (fdesc->repeat == IOP_R_OPTIONAL && !iop_field_is_class(fdesc)) {
        out = iop_field_set_present(env->mp, fdesc, out);
    }

    if (!struct_or_union && data->tag.s) {
        sb_setf(&env->err.buf, "specifying a tag on %s is not allowed",
                yaml_data_get_type(data, true));
        env->err.data = data;
        goto err;
    }

    switch (data->type) {
      case YAML_DATA_SCALAR:
        switch (yaml_scalar_to_iop_field(env, data, fdesc, in_array, out)) {
          case YUNPACK_OK:
            break;
          case YUNPACK_INVALID_B64_VAL:
            sb_setf(&env->err.buf, "the value must be encoded in base64");
            env->err.data = data;
            goto err;
          case YUNPACK_INVALID_ENUM_VAL:
            sb_setf(&env->err.buf,
                    "the value is not valid for the enum `%pL`",
                    &fdesc->u1.en_desc->name);
            env->err.data = data;
            goto err;
          case YUNPACK_TYPE_MISMATCH:
            yaml_set_type_mismatch_err(env, data, fdesc);
            goto err;
          case YUNPACK_OOB:
            sb_setf(&env->err.buf,
                    "the value is out of range for the field of type %s",
                    iop_type_get_string_desc(fdesc->type));
            env->err.data = data;
            goto err;
          default:
            assert (false);
            return -1;
        }
        break;

      case YAML_DATA_OBJ:
        if (struct_or_union) {
            if (yaml_data_to_typed_struct(env, data, fdesc->u1.st_desc,
                                          out) < 0)
            {
                goto err;
            }
        } else {
            yaml_set_type_mismatch_err(env, data, fdesc);
            goto err;
        }
        break;

      case YAML_DATA_SEQ:
        if (yaml_seq_to_iop_field(env, data, st_desc, fdesc, out) < 0) {
            goto err;
        }
        break;

      default:
        assert (false);
        return -1;
    }

    logger_trace(&_G.logger, 2,
                 "unpack %s from "YAML_POS_FMT" up to "YAML_POS_FMT
                 " into field %pL of struct %pL",
                 yaml_data_get_type(data, false),
                 YAML_POS_ARG(data->pos_start), YAML_POS_ARG(data->pos_end),
                 &fdesc->name, &st_desc->fullname);
    return 0;

  err:
    sb_prependf(&env->err.buf, "cannot set field `%pL`: ", &fdesc->name);
    return -1;
}

/* }}} */

static void yunpack_err_pretty_print(const yunpack_error_t *err,
                                     const iop_struct_t * nonnull st,
                                     const char * nullable filename,
                                     const pstream_t *full_input, sb_t *out)
{
    pstream_t ps;
    bool one_liner;

    if (filename) {
        sb_addf(out, "%s:", filename);
    }
    sb_addf(out, YAML_POS_FMT": %pL", YAML_POS_ARG(err->data->pos_start),
            &err->buf);

    one_liner = err->data->pos_end.line_nb
             == err->data->pos_start.line_nb;

    /* get the full line including pos_start */
    ps.s = err->data->pos_start.s;
    ps.s -= err->data->pos_start.col_nb - 1;

    /* find the end of the line */
    ps.s_end = one_liner ? err->data->pos_end.s - 1 : ps.s;
    while (ps.s_end < full_input->s_end && *ps.s_end != '\n') {
        ps.s_end++;
    }
    /* print the whole line */
    sb_addf(out, "\n%*pM\n", PS_FMT_ARG(&ps));

    /* then display some indications or where the issue is */
    for (unsigned i = 1; i < err->data->pos_start.col_nb; i++) {
        sb_addc(out, ' ');
    }
    if (one_liner) {
        for (unsigned i = err->data->pos_start.col_nb;
             i < err->data->pos_end.col_nb; i++)
        {
            sb_addc(out, '^');
        }
    } else {
        sb_adds(out, "^ starting here");
    }
}

static int
_t_iop_yunpack_ps(pstream_t * nonnull ps, const iop_struct_t * nonnull st,
                  const char * nullable filename, void * nonnull out,
                  sb_t * nonnull out_err)
{
    t_SB_1k(err);
    yunpack_env_t unpack_env;
    yaml_data_t data;

    RETHROW(t_yaml_parse(*ps, &data, out_err));

    p_clear(&unpack_env, 1);
    unpack_env.mp = t_pool();
    unpack_env.err.buf = err;
    /* The YAML packer is made for public interfaces. Use flags that makes
     * sense in this context. In the future, they might be overridden if
     * some internal use-cases are found. */
    unpack_env.flags = IOP_UNPACK_FORBID_PRIVATE;
    if (yaml_data_to_typed_struct(&unpack_env, &data, st, out) < 0) {
        yunpack_err_pretty_print(&unpack_env.err, st, filename, ps, out_err);
        return -1;
    }

    /* XXX: may be removed in the future, but useful while the code is still
     * young to ensure we did not mess up our unpacking. */
#ifndef NDEBUG
    {
        void *val = iop_struct_is_class(st) ? *(void **)out : out;

        if (!expect(iop_check_constraints_desc(st, val) >= 0)) {
            sb_setf(&unpack_env.err.buf, "invalid object: %s", iop_get_err());
            unpack_env.err.data = &data;
            yunpack_err_pretty_print(&unpack_env.err, st, filename, ps,
                                     out_err);
            return -1;
        }
    }
#endif

    return 0;
}

int t_iop_yunpack_ps(pstream_t * nonnull ps, const iop_struct_t * nonnull st,
                     void * nonnull out, sb_t * nonnull out_err)
{
    return _t_iop_yunpack_ps(ps, st, NULL, out, out_err);
}

static void * nonnull t_alloc_st_out(const iop_struct_t * nonnull st,
                                     void * nullable * nonnull out)
{
    if (iop_struct_is_class(st)) {
        /* "out" will be (re)allocated after, when the real packed class type
         * will be known. */
        return out;
    } else {
        *out = mp_irealloc(t_pool(), *out, 0, st->size, 8, MEM_RAW);
        return *out;
    }
}

int
t_iop_yunpack_ptr_ps(pstream_t * nonnull ps, const iop_struct_t * nonnull st,
                     void * nullable * nonnull out, sb_t * nonnull out_err)
{
    return t_iop_yunpack_ps(ps, st, t_alloc_st_out(st, out), out_err);
}

int t_iop_yunpack_file(const char * nonnull filename,
                       const iop_struct_t * nonnull st,
                       void * nullable * nonnull out,
                       sb_t * nonnull out_err)
{
    lstr_t file = LSTR_NULL_V;
    pstream_t ps;
    int res = 0;

    if (lstr_init_from_file(&file, filename, PROT_READ, MAP_SHARED) < 0) {
        sb_setf(out_err, "cannot read file %s: %m", filename);
        return -1;
    }

    ps = ps_initlstr(&file);
    res = _t_iop_yunpack_ps(&ps, st, filename, out, out_err);
    lstr_wipe(&file);

    return res;
}

/** Convert a YAML file into an IOP C structure using the t_pool().
 *
 * See t_iop_junpack_ptr_ps.
 */
__must_check__
int t_iop_yunpack_ptr_file(const char * nonnull filename,
                           const iop_struct_t * nonnull st,
                           void * nullable * nonnull out,
                           sb_t * nonnull out_err)
{
    return t_iop_yunpack_file(filename, st, t_alloc_st_out(st, out), out_err);
}

/* }}} */
/* {{{ ypack */

static void
t_iop_struct_to_yaml_data(const iop_struct_t * nonnull desc,
                          const void * nonnull value, int flags,
                          yaml_data_t * nonnull data);

static void
t_iop_field_to_yaml_data(const iop_field_t * nonnull fdesc,
                         const void * nonnull ptr, int j, int flags,
                         yaml_data_t * nonnull data)
{
    p_clear(data, 1);

    switch (fdesc->type) {
#define CASE(n) \
      case IOP_T_I##n:                                                       \
        yaml_data_set_int(data, IOP_FIELD(int##n##_t, ptr, j));              \
        break;                                                               \
      case IOP_T_U##n:                                                       \
        yaml_data_set_uint(data, IOP_FIELD(uint##n##_t, ptr, j));            \
        break;
      CASE(8); CASE(16); CASE(32); CASE(64);
#undef CASE

      case IOP_T_ENUM: {
        const void *v;

        v = iop_enum_to_str_desc(fdesc->u1.en_desc,
                                 IOP_FIELD(int, ptr, j)).s;
        if (likely(v)) {
            yaml_data_set_string(data, LSTR(v));
        } else {
            yaml_data_set_int(data, IOP_FIELD(int, ptr, j));
        }
      } break;

      case IOP_T_BOOL:
        yaml_data_set_bool(data, IOP_FIELD(bool, ptr, j));
        break;

      case IOP_T_DOUBLE:
        yaml_data_set_double(data, IOP_FIELD(double, ptr, j));
        break;

      case IOP_T_UNION:
      case IOP_T_STRUCT: {
        const void *v = iop_json_get_struct_field_value(fdesc, ptr, j);

        t_iop_struct_to_yaml_data(fdesc->u1.st_desc, v, flags, data);
      } break;

      case IOP_T_STRING:
      case IOP_T_XML:
      case IOP_T_DATA: {
        lstr_t sv = IOP_FIELD(const lstr_t, ptr, j);

        if (fdesc->type == IOP_T_DATA && sv.len > 0) {
            t_SB_1k(sb);

            sb_reset(&sb);
            sb_addlstr_b64(&sb, sv, -1);
            yaml_data_set_string(data, LSTR_SB_V(&sb));
        } else {
            yaml_data_set_string(data, sv);
        }
      } break;

      case IOP_T_VOID:
        yaml_data_set_null(data);
        break;

      default:
        abort();
    }
}

static void
t_append_iop_struct_to_fields(const iop_struct_t * nonnull desc,
                              const void * nonnull value, int flags,
                              yaml_data_t *data)
{
    const iop_field_t *fstart;
    const iop_field_t *fend;

    if (desc->is_union) {
        fstart = get_union_field(desc, value);
        fend = fstart + 1;
    } else {
        fstart = desc->fields;
        fend = desc->fields + desc->fields_len;
    }

    for (const iop_field_t *fdesc = fstart; fdesc < fend; fdesc++) {
        bool repeated = fdesc->repeat == IOP_R_REPEATED;
        const void *ptr;
        bool is_skipped = false;
        yaml_data_t field_data;
        int n;

        ptr = iop_json_get_n_and_ptr(desc, flags, fdesc, value, &n,
                                     &is_skipped);
        if (is_skipped) {
            continue;
        }

        if (n == 1 && !repeated) {
            t_iop_field_to_yaml_data(fdesc, ptr, 0, flags, &field_data);
        } else {
            t_yaml_data_new_seq(&field_data, n);

            for (int j = 0; j < n; j++) {
                yaml_data_t elem;

                t_iop_field_to_yaml_data(fdesc, ptr, j, flags, &elem);
                yaml_seq_add_data(&field_data, elem);
            }
        }

        yaml_obj_add_field(data, lstr_dupc(fdesc->name), field_data);
    }
}

static void
t_iop_struct_to_yaml_data(const iop_struct_t * nonnull desc,
                          const void * nonnull value, int flags,
                          yaml_data_t * nonnull data)
{
    if (iop_struct_is_class(desc)) {
        qv_t(iop_struct) parents;
        const iop_struct_t *real_desc = *(const iop_struct_t **)value;
        int nb_fields = 0;
        lstr_t tag = LSTR_NULL_V;

        e_assert(panic, !real_desc->class_attrs->is_abstract,
                 "packing of abstract class '%*pM' is forbidden",
                 LSTR_FMT_ARG(real_desc->fullname));

        /* If this assert fails, you are exporting private classes through
         * a public interface... this is BAD!
         */
        assert (!real_desc->class_attrs->is_private);

        /* Write type of class */
        if (desc != real_desc
        ||  !(flags & IOP_JPACK_SKIP_OPTIONAL_CLASS_NAMES))
        {
            tag = lstr_dupc(real_desc->fullname);
        }

        /* We want to write the fields in the order "master -> children", and
         * not "children -> master", so first build a qvector of the parents.
         */
        qv_inita(&parents, 8);
        do {
            qv_append(&parents, real_desc);
            nb_fields += real_desc->fields_len;
            real_desc = real_desc->class_attrs->parent;
        } while (real_desc);

        /* Write fields of different levels */
        t_yaml_data_new_obj(data, nb_fields);
        for (int pos = parents.len; pos-- > 0; ) {
            t_append_iop_struct_to_fields(parents.tab[pos], value, flags,
                                          data);
        }
        qv_wipe(&parents);

        data->tag = tag;
    } else {
        t_yaml_data_new_obj(data, desc->fields_len);
        t_append_iop_struct_to_fields(desc, value, flags, data);
    }
}

#define DEFAULT_PACK_FLAGS                                                   \
        IOP_JPACK_SKIP_PRIVATE                                               \
      | IOP_JPACK_SKIP_DEFAULT                                               \
      | IOP_JPACK_SKIP_EMPTY_ARRAYS                                          \
      | IOP_JPACK_SKIP_EMPTY_STRUCTS                                         \
      | IOP_JPACK_SKIP_OPTIONAL_CLASS_NAMES

int iop_sb_ypack_with_flags(sb_t * nonnull sb,
                            const iop_struct_t * nonnull st,
                            const void * nonnull value, unsigned flags)
{
    yaml_data_t data;

    t_iop_struct_to_yaml_data(st, value, flags, &data);

    return yaml_pack_sb(&data, sb);
}

int iop_sb_ypack(sb_t * nonnull sb, const iop_struct_t * nonnull st,
                 const void * nonnull value)
{
    return iop_sb_ypack_with_flags(sb, st, value, DEFAULT_PACK_FLAGS);
}

int (iop_ypack_file)(const char *filename, unsigned file_flags,
                     mode_t file_mode, const iop_struct_t *st,
                     const void *value, sb_t *err)
{
    yaml_data_t data;

    t_iop_struct_to_yaml_data(st, value, DEFAULT_PACK_FLAGS, &data);

    return yaml_pack_file(filename, file_flags, file_mode, &data, err);
}

/* }}} */
/* {{{ Module */

static int iop_yaml_initialize(void *arg)
{
    return 0;
}

static int iop_yaml_shutdown(void)
{
    return 0;
}

MODULE_BEGIN(iop_yaml)
    /* There is an implicit dependency on "log" */
MODULE_END()

/* }}} */
