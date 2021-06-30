/*
 * M-profile MVE Operations
 *
 * Copyright (c) 2021 Linaro, Ltd.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 */

#include "qemu/osdep.h"
#include "qemu/int128.h"
#include "cpu.h"
#include "internals.h"
#include "vec_internal.h"
#include "exec/helper-proto.h"
#include "exec/cpu_ldst.h"
#include "exec/exec-all.h"
#include "tcg/tcg.h"

static uint16_t mve_element_mask(CPUARMState *env)
{
    /*
     * Return the mask of which elements in the MVE vector should be
     * updated. This is a combination of multiple things:
     *  (1) by default, we update every lane in the vector
     *  (2) VPT predication stores its state in the VPR register;
     *  (3) low-overhead-branch tail predication will mask out part
     *      the vector on the final iteration of the loop
     *  (4) if EPSR.ECI is set then we must execute only some beats
     *      of the insn
     * We combine all these into a 16-bit result with the same semantics
     * as VPR.P0: 0 to mask the lane, 1 if it is active.
     * 8-bit vector ops will look at all bits of the result;
     * 16-bit ops will look at bits 0, 2, 4, ...;
     * 32-bit ops will look at bits 0, 4, 8 and 12.
     * Compare pseudocode GetCurInstrBeat(), though that only returns
     * the 4-bit slice of the mask corresponding to a single beat.
     */
    uint16_t mask = FIELD_EX32(env->v7m.vpr, V7M_VPR, P0);

    if (!(env->v7m.vpr & R_V7M_VPR_MASK01_MASK)) {
        mask |= 0xff;
    }
    if (!(env->v7m.vpr & R_V7M_VPR_MASK23_MASK)) {
        mask |= 0xff00;
    }

    if (env->v7m.ltpsize < 4 &&
        env->regs[14] <= (1 << (4 - env->v7m.ltpsize))) {
        /*
         * Tail predication active, and this is the last loop iteration.
         * The element size is (1 << ltpsize), and we only want to process
         * loopcount elements, so we want to retain the least significant
         * (loopcount * esize) predicate bits and zero out bits above that.
         */
        int masklen = env->regs[14] << env->v7m.ltpsize;
        assert(masklen <= 16);
        mask &= MAKE_64BIT_MASK(0, masklen);
    }

    if ((env->condexec_bits & 0xf) == 0) {
        /*
         * ECI bits indicate which beats are already executed;
         * we handle this by effectively predicating them out.
         */
        int eci = env->condexec_bits >> 4;
        switch (eci) {
        case ECI_NONE:
            break;
        case ECI_A0:
            mask &= 0xfff0;
            break;
        case ECI_A0A1:
            mask &= 0xff00;
            break;
        case ECI_A0A1A2:
        case ECI_A0A1A2B0:
            mask &= 0xf000;
            break;
        default:
            g_assert_not_reached();
        }
    }

    return mask;
}

static void mve_advance_vpt(CPUARMState *env)
{
    /* Advance the VPT and ECI state if necessary */
    uint32_t vpr = env->v7m.vpr;
    unsigned mask01, mask23;

    if ((env->condexec_bits & 0xf) == 0) {
        env->condexec_bits = (env->condexec_bits == (ECI_A0A1A2B0 << 4)) ?
            (ECI_A0 << 4) : (ECI_NONE << 4);
    }

    if (!(vpr & (R_V7M_VPR_MASK01_MASK | R_V7M_VPR_MASK23_MASK))) {
        /* VPT not enabled, nothing to do */
        return;
    }

    mask01 = FIELD_EX32(vpr, V7M_VPR, MASK01);
    mask23 = FIELD_EX32(vpr, V7M_VPR, MASK23);
    if (mask01 > 8) {
        /* high bit set, but not 0b1000: invert the relevant half of P0 */
        vpr ^= 0xff;
    }
    if (mask23 > 8) {
        /* high bit set, but not 0b1000: invert the relevant half of P0 */
        vpr ^= 0xff00;
    }
    vpr = FIELD_DP32(vpr, V7M_VPR, MASK01, mask01 << 1);
    vpr = FIELD_DP32(vpr, V7M_VPR, MASK23, mask23 << 1);
    env->v7m.vpr = vpr;
}


#define DO_VLDR(OP, MSIZE, LDTYPE, ESIZE, TYPE)                         \
    void HELPER(mve_##OP)(CPUARMState *env, void *vd, uint32_t addr)    \
    {                                                                   \
        TYPE *d = vd;                                                   \
        uint16_t mask = mve_element_mask(env);                          \
        unsigned b, e;                                                  \
        /*                                                              \
         * R_SXTM allows the dest reg to become UNKNOWN for abandoned   \
         * beats so we don't care if we update part of the dest and     \
         * then take an exception.                                      \
         */                                                             \
        for (b = 0, e = 0; b < 16; b += ESIZE, e++) {                   \
            if (mask & (1 << b)) {                                      \
                d[H##ESIZE(e)] = cpu_##LDTYPE##_data_ra(env, addr, GETPC()); \
            }                                                           \
            addr += MSIZE;                                              \
        }                                                               \
        mve_advance_vpt(env);                                           \
    }

#define DO_VSTR(OP, MSIZE, STTYPE, ESIZE, TYPE)                         \
    void HELPER(mve_##OP)(CPUARMState *env, void *vd, uint32_t addr)    \
    {                                                                   \
        TYPE *d = vd;                                                   \
        uint16_t mask = mve_element_mask(env);                          \
        unsigned b, e;                                                  \
        for (b = 0, e = 0; b < 16; b += ESIZE, e++) {                   \
            if (mask & (1 << b)) {                                      \
                cpu_##STTYPE##_data_ra(env, addr, d[H##ESIZE(e)], GETPC()); \
            }                                                           \
            addr += MSIZE;                                              \
        }                                                               \
        mve_advance_vpt(env);                                           \
    }

DO_VLDR(vldrb, 1, ldub, 1, uint8_t)
DO_VLDR(vldrh, 2, lduw, 2, uint16_t)
DO_VLDR(vldrw, 4, ldl, 4, uint32_t)

DO_VSTR(vstrb, 1, stb, 1, uint8_t)
DO_VSTR(vstrh, 2, stw, 2, uint16_t)
DO_VSTR(vstrw, 4, stl, 4, uint32_t)

DO_VLDR(vldrb_sh, 1, ldsb, 2, int16_t)
DO_VLDR(vldrb_sw, 1, ldsb, 4, int32_t)
DO_VLDR(vldrb_uh, 1, ldub, 2, uint16_t)
DO_VLDR(vldrb_uw, 1, ldub, 4, uint32_t)
DO_VLDR(vldrh_sw, 2, ldsw, 4, int32_t)
DO_VLDR(vldrh_uw, 2, lduw, 4, uint32_t)

DO_VSTR(vstrb_h, 1, stb, 2, int16_t)
DO_VSTR(vstrb_w, 1, stb, 4, int32_t)
DO_VSTR(vstrh_w, 2, stw, 4, int32_t)

#undef DO_VLDR
#undef DO_VSTR

/*
 * The mergemask(D, R, M) macro performs the operation "*D = R" but
 * storing only the bytes which correspond to 1 bits in M,
 * leaving other bytes in *D unchanged. We use _Generic
 * to select the correct implementation based on the type of D.
 */

static void mergemask_ub(uint8_t *d, uint8_t r, uint16_t mask)
{
    if (mask & 1) {
        *d = r;
    }
}

static void mergemask_sb(int8_t *d, int8_t r, uint16_t mask)
{
    mergemask_ub((uint8_t *)d, r, mask);
}

static void mergemask_uh(uint16_t *d, uint16_t r, uint16_t mask)
{
    uint16_t bmask = expand_pred_b_data[mask & 3];
    *d = (*d & ~bmask) | (r & bmask);
}

static void mergemask_sh(int16_t *d, int16_t r, uint16_t mask)
{
    mergemask_uh((uint16_t *)d, r, mask);
}

static void mergemask_uw(uint32_t *d, uint32_t r, uint16_t mask)
{
    uint32_t bmask = expand_pred_b_data[mask & 0xf];
    *d = (*d & ~bmask) | (r & bmask);
}

static void mergemask_sw(int32_t *d, int32_t r, uint16_t mask)
{
    mergemask_uw((uint32_t *)d, r, mask);
}

static void mergemask_uq(uint64_t *d, uint64_t r, uint16_t mask)
{
    uint64_t bmask = expand_pred_b_data[mask & 0xff];
    *d = (*d & ~bmask) | (r & bmask);
}

static void mergemask_sq(int64_t *d, int64_t r, uint16_t mask)
{
    mergemask_uq((uint64_t *)d, r, mask);
}

#define mergemask(D, R, M)                      \
    _Generic(D,                                 \
             uint8_t *: mergemask_ub,           \
             int8_t *:  mergemask_sb,           \
             uint16_t *: mergemask_uh,          \
             int16_t *:  mergemask_sh,          \
             uint32_t *: mergemask_uw,          \
             int32_t *:  mergemask_sw,          \
             uint64_t *: mergemask_uq,          \
             int64_t *:  mergemask_sq)(D, R, M)

void HELPER(mve_vdup)(CPUARMState *env, void *vd, uint32_t val)
{
    /*
     * The generated code already replicated an 8 or 16 bit constant
     * into the 32-bit value, so we only need to write the 32-bit
     * value to all elements of the Qreg, allowing for predication.
     */
    uint32_t *d = vd;
    uint16_t mask = mve_element_mask(env);
    unsigned e;
    for (e = 0; e < 16 / 4; e++, mask >>= 4) {
        mergemask(&d[H4(e)], val, mask);
    }
    mve_advance_vpt(env);
}

#define DO_1OP(OP, ESIZE, TYPE, FN)                                     \
    void HELPER(mve_##OP)(CPUARMState *env, void *vd, void *vm)         \
    {                                                                   \
        TYPE *d = vd, *m = vm;                                          \
        uint16_t mask = mve_element_mask(env);                          \
        unsigned e;                                                     \
        for (e = 0; e < 16 / ESIZE; e++, mask >>= ESIZE) {              \
            mergemask(&d[H##ESIZE(e)], FN(m[H##ESIZE(e)]), mask);       \
        }                                                               \
        mve_advance_vpt(env);                                           \
    }

#define DO_CLS_B(N)   (clrsb32(N) - 24)
#define DO_CLS_H(N)   (clrsb32(N) - 16)

DO_1OP(vclsb, 1, int8_t, DO_CLS_B)
DO_1OP(vclsh, 2, int16_t, DO_CLS_H)
DO_1OP(vclsw, 4, int32_t, clrsb32)

#define DO_CLZ_B(N)   (clz32(N) - 24)
#define DO_CLZ_H(N)   (clz32(N) - 16)

DO_1OP(vclzb, 1, uint8_t, DO_CLZ_B)
DO_1OP(vclzh, 2, uint16_t, DO_CLZ_H)
DO_1OP(vclzw, 4, uint32_t, clz32)

DO_1OP(vrev16b, 2, uint16_t, bswap16)
DO_1OP(vrev32b, 4, uint32_t, bswap32)
DO_1OP(vrev32h, 4, uint32_t, hswap32)
DO_1OP(vrev64b, 8, uint64_t, bswap64)
DO_1OP(vrev64h, 8, uint64_t, hswap64)
DO_1OP(vrev64w, 8, uint64_t, wswap64)

#define DO_NOT(N) (~(N))

DO_1OP(vmvn, 8, uint64_t, DO_NOT)

#define DO_ABS(N) ((N) < 0 ? -(N) : (N))
#define DO_FABSH(N)  ((N) & dup_const(MO_16, 0x7fff))
#define DO_FABSS(N)  ((N) & dup_const(MO_32, 0x7fffffff))

DO_1OP(vabsb, 1, int8_t, DO_ABS)
DO_1OP(vabsh, 2, int16_t, DO_ABS)
DO_1OP(vabsw, 4, int32_t, DO_ABS)

/* We can do these 64 bits at a time */
DO_1OP(vfabsh, 8, uint64_t, DO_FABSH)
DO_1OP(vfabss, 8, uint64_t, DO_FABSS)

#define DO_NEG(N)    (-(N))
#define DO_FNEGH(N) ((N) ^ dup_const(MO_16, 0x8000))
#define DO_FNEGS(N) ((N) ^ dup_const(MO_32, 0x80000000))

DO_1OP(vnegb, 1, int8_t, DO_NEG)
DO_1OP(vnegh, 2, int16_t, DO_NEG)
DO_1OP(vnegw, 4, int32_t, DO_NEG)

/* We can do these 64 bits at a time */
DO_1OP(vfnegh, 8, uint64_t, DO_FNEGH)
DO_1OP(vfnegs, 8, uint64_t, DO_FNEGS)

#define DO_2OP(OP, ESIZE, TYPE, FN)                                     \
    void HELPER(glue(mve_, OP))(CPUARMState *env,                       \
                                void *vd, void *vn, void *vm)           \
    {                                                                   \
        TYPE *d = vd, *n = vn, *m = vm;                                 \
        uint16_t mask = mve_element_mask(env);                          \
        unsigned e;                                                     \
        for (e = 0; e < 16 / ESIZE; e++, mask >>= ESIZE) {              \
            mergemask(&d[H##ESIZE(e)],                                  \
                      FN(n[H##ESIZE(e)], m[H##ESIZE(e)]), mask);        \
        }                                                               \
        mve_advance_vpt(env);                                           \
    }

/* provide unsigned 2-op helpers for all sizes */
#define DO_2OP_U(OP, FN)                        \
    DO_2OP(OP##b, 1, uint8_t, FN)               \
    DO_2OP(OP##h, 2, uint16_t, FN)              \
    DO_2OP(OP##w, 4, uint32_t, FN)

/* provide signed 2-op helpers for all sizes */
#define DO_2OP_S(OP, FN)                        \
    DO_2OP(OP##b, 1, int8_t, FN)                \
    DO_2OP(OP##h, 2, int16_t, FN)               \
    DO_2OP(OP##w, 4, int32_t, FN)

/*
 * "Long" operations where two half-sized inputs (taken from either the
 * top or the bottom of the input vector) produce a double-width result.
 * Here ESIZE, TYPE are for the input, and LESIZE, LTYPE for the output.
 */
#define DO_2OP_L(OP, TOP, ESIZE, TYPE, LESIZE, LTYPE, FN)               \
    void HELPER(glue(mve_, OP))(CPUARMState *env, void *vd, void *vn, void *vm) \
    {                                                                   \
        LTYPE *d = vd;                                                  \
        TYPE *n = vn, *m = vm;                                          \
        uint16_t mask = mve_element_mask(env);                          \
        unsigned le;                                                    \
        for (le = 0; le < 16 / LESIZE; le++, mask >>= LESIZE) {         \
            LTYPE r = FN((LTYPE)n[H##ESIZE(le * 2 + TOP)],              \
                         m[H##ESIZE(le * 2 + TOP)]);                    \
            mergemask(&d[H##LESIZE(le)], r, mask);                      \
        }                                                               \
        mve_advance_vpt(env);                                           \
    }

#define DO_2OP_SAT(OP, ESIZE, TYPE, FN)                                 \
    void HELPER(glue(mve_, OP))(CPUARMState *env, void *vd, void *vn, void *vm) \
    {                                                                   \
        TYPE *d = vd, *n = vn, *m = vm;                                 \
        uint16_t mask = mve_element_mask(env);                          \
        unsigned e;                                                     \
        bool qc = false;                                                \
        for (e = 0; e < 16 / ESIZE; e++, mask >>= ESIZE) {              \
            bool sat = false;                                           \
            TYPE r = FN(n[H##ESIZE(e)], m[H##ESIZE(e)], &sat);          \
            mergemask(&d[H##ESIZE(e)], r, mask);                        \
            qc |= sat & mask & 1;                                       \
        }                                                               \
        if (qc) {                                                       \
            env->vfp.qc[0] = qc;                                        \
        }                                                               \
        mve_advance_vpt(env);                                           \
    }

/* provide unsigned 2-op helpers for all sizes */
#define DO_2OP_SAT_U(OP, FN)                    \
    DO_2OP_SAT(OP##b, 1, uint8_t, FN)           \
    DO_2OP_SAT(OP##h, 2, uint16_t, FN)          \
    DO_2OP_SAT(OP##w, 4, uint32_t, FN)

/* provide signed 2-op helpers for all sizes */
#define DO_2OP_SAT_S(OP, FN)                    \
    DO_2OP_SAT(OP##b, 1, int8_t, FN)            \
    DO_2OP_SAT(OP##h, 2, int16_t, FN)           \
    DO_2OP_SAT(OP##w, 4, int32_t, FN)

#define DO_AND(N, M)  ((N) & (M))
#define DO_BIC(N, M)  ((N) & ~(M))
#define DO_ORR(N, M)  ((N) | (M))
#define DO_ORN(N, M)  ((N) | ~(M))
#define DO_EOR(N, M)  ((N) ^ (M))

DO_2OP(vand, 8, uint64_t, DO_AND)
DO_2OP(vbic, 8, uint64_t, DO_BIC)
DO_2OP(vorr, 8, uint64_t, DO_ORR)
DO_2OP(vorn, 8, uint64_t, DO_ORN)
DO_2OP(veor, 8, uint64_t, DO_EOR)

#define DO_ADD(N, M) ((N) + (M))
#define DO_SUB(N, M) ((N) - (M))
#define DO_MUL(N, M) ((N) * (M))

DO_2OP_U(vadd, DO_ADD)
DO_2OP_U(vsub, DO_SUB)
DO_2OP_U(vmul, DO_MUL)

DO_2OP_L(vmullbsb, 0, 1, int8_t, 2, int16_t, DO_MUL)
DO_2OP_L(vmullbsh, 0, 2, int16_t, 4, int32_t, DO_MUL)
DO_2OP_L(vmullbsw, 0, 4, int32_t, 8, int64_t, DO_MUL)
DO_2OP_L(vmullbub, 0, 1, uint8_t, 2, uint16_t, DO_MUL)
DO_2OP_L(vmullbuh, 0, 2, uint16_t, 4, uint32_t, DO_MUL)
DO_2OP_L(vmullbuw, 0, 4, uint32_t, 8, uint64_t, DO_MUL)

DO_2OP_L(vmulltsb, 1, 1, int8_t, 2, int16_t, DO_MUL)
DO_2OP_L(vmulltsh, 1, 2, int16_t, 4, int32_t, DO_MUL)
DO_2OP_L(vmulltsw, 1, 4, int32_t, 8, int64_t, DO_MUL)
DO_2OP_L(vmulltub, 1, 1, uint8_t, 2, uint16_t, DO_MUL)
DO_2OP_L(vmulltuh, 1, 2, uint16_t, 4, uint32_t, DO_MUL)
DO_2OP_L(vmulltuw, 1, 4, uint32_t, 8, uint64_t, DO_MUL)

/*
 * Because the computation type is at least twice as large as required,
 * these work for both signed and unsigned source types.
 */
static inline uint8_t do_mulh_b(int32_t n, int32_t m)
{
    return (n * m) >> 8;
}

static inline uint16_t do_mulh_h(int32_t n, int32_t m)
{
    return (n * m) >> 16;
}

static inline uint32_t do_mulh_w(int64_t n, int64_t m)
{
    return (n * m) >> 32;
}

static inline uint8_t do_rmulh_b(int32_t n, int32_t m)
{
    return (n * m + (1U << 7)) >> 8;
}

static inline uint16_t do_rmulh_h(int32_t n, int32_t m)
{
    return (n * m + (1U << 15)) >> 16;
}

static inline uint32_t do_rmulh_w(int64_t n, int64_t m)
{
    return (n * m + (1U << 31)) >> 32;
}

DO_2OP(vmulhsb, 1, int8_t, do_mulh_b)
DO_2OP(vmulhsh, 2, int16_t, do_mulh_h)
DO_2OP(vmulhsw, 4, int32_t, do_mulh_w)
DO_2OP(vmulhub, 1, uint8_t, do_mulh_b)
DO_2OP(vmulhuh, 2, uint16_t, do_mulh_h)
DO_2OP(vmulhuw, 4, uint32_t, do_mulh_w)

DO_2OP(vrmulhsb, 1, int8_t, do_rmulh_b)
DO_2OP(vrmulhsh, 2, int16_t, do_rmulh_h)
DO_2OP(vrmulhsw, 4, int32_t, do_rmulh_w)
DO_2OP(vrmulhub, 1, uint8_t, do_rmulh_b)
DO_2OP(vrmulhuh, 2, uint16_t, do_rmulh_h)
DO_2OP(vrmulhuw, 4, uint32_t, do_rmulh_w)

#define DO_MAX(N, M)  ((N) >= (M) ? (N) : (M))
#define DO_MIN(N, M)  ((N) >= (M) ? (M) : (N))

DO_2OP_S(vmaxs, DO_MAX)
DO_2OP_U(vmaxu, DO_MAX)
DO_2OP_S(vmins, DO_MIN)
DO_2OP_U(vminu, DO_MIN)

#define DO_ABD(N, M)  ((N) >= (M) ? (N) - (M) : (M) - (N))

DO_2OP_S(vabds, DO_ABD)
DO_2OP_U(vabdu, DO_ABD)

static inline uint32_t do_vhadd_u(uint32_t n, uint32_t m)
{
    return ((uint64_t)n + m) >> 1;
}

static inline int32_t do_vhadd_s(int32_t n, int32_t m)
{
    return ((int64_t)n + m) >> 1;
}

static inline uint32_t do_vhsub_u(uint32_t n, uint32_t m)
{
    return ((uint64_t)n - m) >> 1;
}

static inline int32_t do_vhsub_s(int32_t n, int32_t m)
{
    return ((int64_t)n - m) >> 1;
}

DO_2OP_S(vhadds, do_vhadd_s)
DO_2OP_U(vhaddu, do_vhadd_u)
DO_2OP_S(vhsubs, do_vhsub_s)
DO_2OP_U(vhsubu, do_vhsub_u)

#define DO_VSHLS(N, M) do_sqrshl_bhs(N, (int8_t)(M), sizeof(N) * 8, false, NULL)
#define DO_VSHLU(N, M) do_uqrshl_bhs(N, (int8_t)(M), sizeof(N) * 8, false, NULL)
#define DO_VRSHLS(N, M) do_sqrshl_bhs(N, (int8_t)(M), sizeof(N) * 8, true, NULL)
#define DO_VRSHLU(N, M) do_uqrshl_bhs(N, (int8_t)(M), sizeof(N) * 8, true, NULL)

DO_2OP_S(vshls, DO_VSHLS)
DO_2OP_U(vshlu, DO_VSHLU)
DO_2OP_S(vrshls, DO_VRSHLS)
DO_2OP_U(vrshlu, DO_VRSHLU)

#define DO_RHADD_S(N, M) (((int64_t)(N) + (M) + 1) >> 1)
#define DO_RHADD_U(N, M) (((uint64_t)(N) + (M) + 1) >> 1)

DO_2OP_S(vrhadds, DO_RHADD_S)
DO_2OP_U(vrhaddu, DO_RHADD_U)

static void do_vadc(CPUARMState *env, uint32_t *d, uint32_t *n, uint32_t *m,
                    uint32_t inv, uint32_t carry_in, bool update_flags)
{
    uint16_t mask = mve_element_mask(env);
    unsigned e;

    /* If any additions trigger, we will update flags. */
    if (mask & 0x1111) {
        update_flags = true;
    }

    for (e = 0; e < 16 / 4; e++, mask >>= 4) {
        uint64_t r = carry_in;
        r += n[H4(e)];
        r += m[H4(e)] ^ inv;
        if (mask & 1) {
            carry_in = r >> 32;
        }
        mergemask(&d[H4(e)], r, mask);
    }

    if (update_flags) {
        /* Store C, clear NZV. */
        env->vfp.xregs[ARM_VFP_FPSCR] &= ~FPCR_NZCV_MASK;
        env->vfp.xregs[ARM_VFP_FPSCR] |= carry_in * FPCR_C;
    }
    mve_advance_vpt(env);
}

void HELPER(mve_vadc)(CPUARMState *env, void *vd, void *vn, void *vm)
{
    bool carry_in = env->vfp.xregs[ARM_VFP_FPSCR] & FPCR_C;
    do_vadc(env, vd, vn, vm, 0, carry_in, false);
}

void HELPER(mve_vsbc)(CPUARMState *env, void *vd, void *vn, void *vm)
{
    bool carry_in = env->vfp.xregs[ARM_VFP_FPSCR] & FPCR_C;
    do_vadc(env, vd, vn, vm, -1, carry_in, false);
}


void HELPER(mve_vadci)(CPUARMState *env, void *vd, void *vn, void *vm)
{
    do_vadc(env, vd, vn, vm, 0, 0, true);
}

void HELPER(mve_vsbci)(CPUARMState *env, void *vd, void *vn, void *vm)
{
    do_vadc(env, vd, vn, vm, -1, 1, true);
}

#define DO_VCADD(OP, ESIZE, TYPE, FN0, FN1)                             \
    void HELPER(glue(mve_, OP))(CPUARMState *env, void *vd, void *vn, void *vm) \
    {                                                                   \
        TYPE *d = vd, *n = vn, *m = vm;                                 \
        uint16_t mask = mve_element_mask(env);                          \
        unsigned e;                                                     \
        TYPE r[16 / ESIZE];                                             \
        /* Calculate all results first to avoid overwriting inputs */   \
        for (e = 0; e < 16 / ESIZE; e++) {                              \
            if (!(e & 1)) {                                             \
                r[e] = FN0(n[H##ESIZE(e)], m[H##ESIZE(e + 1)]);         \
            } else {                                                    \
                r[e] = FN1(n[H##ESIZE(e)], m[H##ESIZE(e - 1)]);         \
            }                                                           \
        }                                                               \
        for (e = 0; e < 16 / ESIZE; e++, mask >>= ESIZE) {              \
            mergemask(&d[H##ESIZE(e)], r[e], mask);                     \
        }                                                               \
        mve_advance_vpt(env);                                           \
    }

#define DO_VCADD_ALL(OP, FN0, FN1)              \
    DO_VCADD(OP##b, 1, int8_t, FN0, FN1)        \
    DO_VCADD(OP##h, 2, int16_t, FN0, FN1)       \
    DO_VCADD(OP##w, 4, int32_t, FN0, FN1)

DO_VCADD_ALL(vcadd90, DO_SUB, DO_ADD)
DO_VCADD_ALL(vcadd270, DO_ADD, DO_SUB)
DO_VCADD_ALL(vhcadd90, do_vhsub_s, do_vhadd_s)
DO_VCADD_ALL(vhcadd270, do_vhadd_s, do_vhsub_s)

static inline int32_t do_sat_bhw(int64_t val, int64_t min, int64_t max, bool *s)
{
    if (val > max) {
        *s = true;
        return max;
    } else if (val < min) {
        *s = true;
        return min;
    }
    return val;
}

#define DO_SQADD_B(n, m, s) do_sat_bhw((int64_t)n + m, INT8_MIN, INT8_MAX, s)
#define DO_SQADD_H(n, m, s) do_sat_bhw((int64_t)n + m, INT16_MIN, INT16_MAX, s)
#define DO_SQADD_W(n, m, s) do_sat_bhw((int64_t)n + m, INT32_MIN, INT32_MAX, s)

#define DO_UQADD_B(n, m, s) do_sat_bhw((int64_t)n + m, 0, UINT8_MAX, s)
#define DO_UQADD_H(n, m, s) do_sat_bhw((int64_t)n + m, 0, UINT16_MAX, s)
#define DO_UQADD_W(n, m, s) do_sat_bhw((int64_t)n + m, 0, UINT32_MAX, s)

#define DO_SQSUB_B(n, m, s) do_sat_bhw((int64_t)n - m, INT8_MIN, INT8_MAX, s)
#define DO_SQSUB_H(n, m, s) do_sat_bhw((int64_t)n - m, INT16_MIN, INT16_MAX, s)
#define DO_SQSUB_W(n, m, s) do_sat_bhw((int64_t)n - m, INT32_MIN, INT32_MAX, s)

#define DO_UQSUB_B(n, m, s) do_sat_bhw((int64_t)n - m, 0, UINT8_MAX, s)
#define DO_UQSUB_H(n, m, s) do_sat_bhw((int64_t)n - m, 0, UINT16_MAX, s)
#define DO_UQSUB_W(n, m, s) do_sat_bhw((int64_t)n - m, 0, UINT32_MAX, s)

/*
 * For QDMULH and QRDMULH we simplify "double and shift by esize" into
 * "shift by esize-1", adjusting the QRDMULH rounding constant to match.
 */
#define DO_QDMULH_B(n, m, s) do_sat_bhw(((int64_t)n * m) >> 7, \
                                        INT8_MIN, INT8_MAX, s)
#define DO_QDMULH_H(n, m, s) do_sat_bhw(((int64_t)n * m) >> 15, \
                                        INT16_MIN, INT16_MAX, s)
#define DO_QDMULH_W(n, m, s) do_sat_bhw(((int64_t)n * m) >> 31, \
                                        INT32_MIN, INT32_MAX, s)

#define DO_QRDMULH_B(n, m, s) do_sat_bhw(((int64_t)n * m + (1 << 6)) >> 7, \
                                         INT8_MIN, INT8_MAX, s)
#define DO_QRDMULH_H(n, m, s) do_sat_bhw(((int64_t)n * m + (1 << 14)) >> 15, \
                                         INT16_MIN, INT16_MAX, s)
#define DO_QRDMULH_W(n, m, s) do_sat_bhw(((int64_t)n * m + (1 << 30)) >> 31, \
                                         INT32_MIN, INT32_MAX, s)

DO_2OP_SAT(vqdmulhb, 1, int8_t, DO_QDMULH_B)
DO_2OP_SAT(vqdmulhh, 2, int16_t, DO_QDMULH_H)
DO_2OP_SAT(vqdmulhw, 4, int32_t, DO_QDMULH_W)

DO_2OP_SAT(vqrdmulhb, 1, int8_t, DO_QRDMULH_B)
DO_2OP_SAT(vqrdmulhh, 2, int16_t, DO_QRDMULH_H)
DO_2OP_SAT(vqrdmulhw, 4, int32_t, DO_QRDMULH_W)

DO_2OP_SAT(vqaddub, 1, uint8_t, DO_UQADD_B)
DO_2OP_SAT(vqadduh, 2, uint16_t, DO_UQADD_H)
DO_2OP_SAT(vqadduw, 4, uint32_t, DO_UQADD_W)
DO_2OP_SAT(vqaddsb, 1, int8_t, DO_SQADD_B)
DO_2OP_SAT(vqaddsh, 2, int16_t, DO_SQADD_H)
DO_2OP_SAT(vqaddsw, 4, int32_t, DO_SQADD_W)

DO_2OP_SAT(vqsubub, 1, uint8_t, DO_UQSUB_B)
DO_2OP_SAT(vqsubuh, 2, uint16_t, DO_UQSUB_H)
DO_2OP_SAT(vqsubuw, 4, uint32_t, DO_UQSUB_W)
DO_2OP_SAT(vqsubsb, 1, int8_t, DO_SQSUB_B)
DO_2OP_SAT(vqsubsh, 2, int16_t, DO_SQSUB_H)
DO_2OP_SAT(vqsubsw, 4, int32_t, DO_SQSUB_W)

/*
 * This wrapper fixes up the impedance mismatch between do_sqrshl_bhs()
 * and friends wanting a uint32_t* sat and our needing a bool*.
 */
#define WRAP_QRSHL_HELPER(FN, N, M, ROUND, satp)                        \
    ({                                                                  \
        uint32_t su32 = 0;                                              \
        typeof(N) r = FN(N, (int8_t)(M), sizeof(N) * 8, ROUND, &su32);  \
        if (su32) {                                                     \
            *satp = true;                                               \
        }                                                               \
        r;                                                              \
    })

#define DO_SQSHL_OP(N, M, satp) \
    WRAP_QRSHL_HELPER(do_sqrshl_bhs, N, M, false, satp)
#define DO_UQSHL_OP(N, M, satp) \
    WRAP_QRSHL_HELPER(do_uqrshl_bhs, N, M, false, satp)
#define DO_SQRSHL_OP(N, M, satp) \
    WRAP_QRSHL_HELPER(do_sqrshl_bhs, N, M, true, satp)
#define DO_UQRSHL_OP(N, M, satp) \
    WRAP_QRSHL_HELPER(do_uqrshl_bhs, N, M, true, satp)

DO_2OP_SAT_S(vqshls, DO_SQSHL_OP)
DO_2OP_SAT_U(vqshlu, DO_UQSHL_OP)
DO_2OP_SAT_S(vqrshls, DO_SQRSHL_OP)
DO_2OP_SAT_U(vqrshlu, DO_UQRSHL_OP)

/*
 * Multiply add dual returning high half
 * The 'FN' here takes four inputs A, B, C, D, a 0/1 indicator of
 * whether to add the rounding constant, and the pointer to the
 * saturation flag, and should do "(A * B + C * D) * 2 + rounding constant",
 * saturate to twice the input size and return the high half; or
 * (A * B - C * D) etc for VQDMLSDH.
 */
#define DO_VQDMLADH_OP(OP, ESIZE, TYPE, XCHG, ROUND, FN)                \
    void HELPER(glue(mve_, OP))(CPUARMState *env, void *vd, void *vn,   \
                                void *vm)                               \
    {                                                                   \
        TYPE *d = vd, *n = vn, *m = vm;                                 \
        uint16_t mask = mve_element_mask(env);                          \
        unsigned e;                                                     \
        bool qc = false;                                                \
        for (e = 0; e < 16 / ESIZE; e++, mask >>= ESIZE) {              \
            bool sat = false;                                           \
            if ((e & 1) == XCHG) {                                      \
                TYPE r = FN(n[H##ESIZE(e)],                             \
                            m[H##ESIZE(e - XCHG)],                      \
                            n[H##ESIZE(e + (1 - 2 * XCHG))],            \
                            m[H##ESIZE(e + (1 - XCHG))],                \
                            ROUND, &sat);                               \
                mergemask(&d[H##ESIZE(e)], r, mask);                    \
                qc |= sat & mask & 1;                                   \
            }                                                           \
        }                                                               \
        if (qc) {                                                       \
            env->vfp.qc[0] = qc;                                        \
        }                                                               \
        mve_advance_vpt(env);                                           \
    }

static int8_t do_vqdmladh_b(int8_t a, int8_t b, int8_t c, int8_t d,
                            int round, bool *sat)
{
    int64_t r = ((int64_t)a * b + (int64_t)c * d) * 2 + (round << 7);
    return do_sat_bhw(r, INT16_MIN, INT16_MAX, sat) >> 8;
}

static int16_t do_vqdmladh_h(int16_t a, int16_t b, int16_t c, int16_t d,
                             int round, bool *sat)
{
    int64_t r = ((int64_t)a * b + (int64_t)c * d) * 2 + (round << 15);
    return do_sat_bhw(r, INT32_MIN, INT32_MAX, sat) >> 16;
}

static int32_t do_vqdmladh_w(int32_t a, int32_t b, int32_t c, int32_t d,
                             int round, bool *sat)
{
    int64_t m1 = (int64_t)a * b;
    int64_t m2 = (int64_t)c * d;
    int64_t r;
    /*
     * Architecturally we should do the entire add, double, round
     * and then check for saturation. We do three saturating adds,
     * but we need to be careful about the order. If the first
     * m1 + m2 saturates then it's impossible for the *2+rc to
     * bring it back into the non-saturated range. However, if
     * m1 + m2 is negative then it's possible that doing the doubling
     * would take the intermediate result below INT64_MAX and the
     * addition of the rounding constant then brings it back in range.
     * So we add half the rounding constant before doubling rather
     * than adding the rounding constant after the doubling.
     */
    if (sadd64_overflow(m1, m2, &r) ||
        sadd64_overflow(r, (round << 30), &r) ||
        sadd64_overflow(r, r, &r)) {
        *sat = true;
        return r < 0 ? INT32_MAX : INT32_MIN;
    }
    return r >> 32;
}

static int8_t do_vqdmlsdh_b(int8_t a, int8_t b, int8_t c, int8_t d,
                            int round, bool *sat)
{
    int64_t r = ((int64_t)a * b - (int64_t)c * d) * 2 + (round << 7);
    return do_sat_bhw(r, INT16_MIN, INT16_MAX, sat) >> 8;
}

static int16_t do_vqdmlsdh_h(int16_t a, int16_t b, int16_t c, int16_t d,
                             int round, bool *sat)
{
    int64_t r = ((int64_t)a * b - (int64_t)c * d) * 2 + (round << 15);
    return do_sat_bhw(r, INT32_MIN, INT32_MAX, sat) >> 16;
}

static int32_t do_vqdmlsdh_w(int32_t a, int32_t b, int32_t c, int32_t d,
                             int round, bool *sat)
{
    int64_t m1 = (int64_t)a * b;
    int64_t m2 = (int64_t)c * d;
    int64_t r;
    /* The same ordering issue as in do_vqdmladh_w applies here too */
    if (ssub64_overflow(m1, m2, &r) ||
        sadd64_overflow(r, (round << 30), &r) ||
        sadd64_overflow(r, r, &r)) {
        *sat = true;
        return r < 0 ? INT32_MAX : INT32_MIN;
    }
    return r >> 32;
}

DO_VQDMLADH_OP(vqdmladhb, 1, int8_t, 0, 0, do_vqdmladh_b)
DO_VQDMLADH_OP(vqdmladhh, 2, int16_t, 0, 0, do_vqdmladh_h)
DO_VQDMLADH_OP(vqdmladhw, 4, int32_t, 0, 0, do_vqdmladh_w)
DO_VQDMLADH_OP(vqdmladhxb, 1, int8_t, 1, 0, do_vqdmladh_b)
DO_VQDMLADH_OP(vqdmladhxh, 2, int16_t, 1, 0, do_vqdmladh_h)
DO_VQDMLADH_OP(vqdmladhxw, 4, int32_t, 1, 0, do_vqdmladh_w)

DO_VQDMLADH_OP(vqrdmladhb, 1, int8_t, 0, 1, do_vqdmladh_b)
DO_VQDMLADH_OP(vqrdmladhh, 2, int16_t, 0, 1, do_vqdmladh_h)
DO_VQDMLADH_OP(vqrdmladhw, 4, int32_t, 0, 1, do_vqdmladh_w)
DO_VQDMLADH_OP(vqrdmladhxb, 1, int8_t, 1, 1, do_vqdmladh_b)
DO_VQDMLADH_OP(vqrdmladhxh, 2, int16_t, 1, 1, do_vqdmladh_h)
DO_VQDMLADH_OP(vqrdmladhxw, 4, int32_t, 1, 1, do_vqdmladh_w)

DO_VQDMLADH_OP(vqdmlsdhb, 1, int8_t, 0, 0, do_vqdmlsdh_b)
DO_VQDMLADH_OP(vqdmlsdhh, 2, int16_t, 0, 0, do_vqdmlsdh_h)
DO_VQDMLADH_OP(vqdmlsdhw, 4, int32_t, 0, 0, do_vqdmlsdh_w)
DO_VQDMLADH_OP(vqdmlsdhxb, 1, int8_t, 1, 0, do_vqdmlsdh_b)
DO_VQDMLADH_OP(vqdmlsdhxh, 2, int16_t, 1, 0, do_vqdmlsdh_h)
DO_VQDMLADH_OP(vqdmlsdhxw, 4, int32_t, 1, 0, do_vqdmlsdh_w)

DO_VQDMLADH_OP(vqrdmlsdhb, 1, int8_t, 0, 1, do_vqdmlsdh_b)
DO_VQDMLADH_OP(vqrdmlsdhh, 2, int16_t, 0, 1, do_vqdmlsdh_h)
DO_VQDMLADH_OP(vqrdmlsdhw, 4, int32_t, 0, 1, do_vqdmlsdh_w)
DO_VQDMLADH_OP(vqrdmlsdhxb, 1, int8_t, 1, 1, do_vqdmlsdh_b)
DO_VQDMLADH_OP(vqrdmlsdhxh, 2, int16_t, 1, 1, do_vqdmlsdh_h)
DO_VQDMLADH_OP(vqrdmlsdhxw, 4, int32_t, 1, 1, do_vqdmlsdh_w)

#define DO_2OP_SCALAR(OP, ESIZE, TYPE, FN)                              \
    void HELPER(glue(mve_, OP))(CPUARMState *env, void *vd, void *vn,   \
                                uint32_t rm)                            \
    {                                                                   \
        TYPE *d = vd, *n = vn;                                          \
        TYPE m = rm;                                                    \
        uint16_t mask = mve_element_mask(env);                          \
        unsigned e;                                                     \
        for (e = 0; e < 16 / ESIZE; e++, mask >>= ESIZE) {              \
            mergemask(&d[H##ESIZE(e)], FN(n[H##ESIZE(e)], m), mask);    \
        }                                                               \
        mve_advance_vpt(env);                                           \
    }

#define DO_2OP_SAT_SCALAR(OP, ESIZE, TYPE, FN)                          \
    void HELPER(glue(mve_, OP))(CPUARMState *env, void *vd, void *vn,   \
                                uint32_t rm)                            \
    {                                                                   \
        TYPE *d = vd, *n = vn;                                          \
        TYPE m = rm;                                                    \
        uint16_t mask = mve_element_mask(env);                          \
        unsigned e;                                                     \
        bool qc = false;                                                \
        for (e = 0; e < 16 / ESIZE; e++, mask >>= ESIZE) {              \
            bool sat = false;                                           \
            mergemask(&d[H##ESIZE(e)], FN(n[H##ESIZE(e)], m, &sat),     \
                      mask);                                            \
            qc |= sat & mask & 1;                                       \
        }                                                               \
        if (qc) {                                                       \
            env->vfp.qc[0] = qc;                                        \
        }                                                               \
        mve_advance_vpt(env);                                           \
    }

/* provide unsigned 2-op scalar helpers for all sizes */
#define DO_2OP_SCALAR_U(OP, FN)                 \
    DO_2OP_SCALAR(OP##b, 1, uint8_t, FN)        \
    DO_2OP_SCALAR(OP##h, 2, uint16_t, FN)       \
    DO_2OP_SCALAR(OP##w, 4, uint32_t, FN)
#define DO_2OP_SCALAR_S(OP, FN)                 \
    DO_2OP_SCALAR(OP##b, 1, int8_t, FN)         \
    DO_2OP_SCALAR(OP##h, 2, int16_t, FN)        \
    DO_2OP_SCALAR(OP##w, 4, int32_t, FN)

DO_2OP_SCALAR_U(vadd_scalar, DO_ADD)
DO_2OP_SCALAR_U(vsub_scalar, DO_SUB)
DO_2OP_SCALAR_U(vmul_scalar, DO_MUL)
DO_2OP_SCALAR_S(vhadds_scalar, do_vhadd_s)
DO_2OP_SCALAR_U(vhaddu_scalar, do_vhadd_u)
DO_2OP_SCALAR_S(vhsubs_scalar, do_vhsub_s)
DO_2OP_SCALAR_U(vhsubu_scalar, do_vhsub_u)

DO_2OP_SAT_SCALAR(vqaddu_scalarb, 1, uint8_t, DO_UQADD_B)
DO_2OP_SAT_SCALAR(vqaddu_scalarh, 2, uint16_t, DO_UQADD_H)
DO_2OP_SAT_SCALAR(vqaddu_scalarw, 4, uint32_t, DO_UQADD_W)
DO_2OP_SAT_SCALAR(vqadds_scalarb, 1, int8_t, DO_SQADD_B)
DO_2OP_SAT_SCALAR(vqadds_scalarh, 2, int16_t, DO_SQADD_H)
DO_2OP_SAT_SCALAR(vqadds_scalarw, 4, int32_t, DO_SQADD_W)

DO_2OP_SAT_SCALAR(vqsubu_scalarb, 1, uint8_t, DO_UQSUB_B)
DO_2OP_SAT_SCALAR(vqsubu_scalarh, 2, uint16_t, DO_UQSUB_H)
DO_2OP_SAT_SCALAR(vqsubu_scalarw, 4, uint32_t, DO_UQSUB_W)
DO_2OP_SAT_SCALAR(vqsubs_scalarb, 1, int8_t, DO_SQSUB_B)
DO_2OP_SAT_SCALAR(vqsubs_scalarh, 2, int16_t, DO_SQSUB_H)
DO_2OP_SAT_SCALAR(vqsubs_scalarw, 4, int32_t, DO_SQSUB_W)

DO_2OP_SAT_SCALAR(vqdmulh_scalarb, 1, int8_t, DO_QDMULH_B)
DO_2OP_SAT_SCALAR(vqdmulh_scalarh, 2, int16_t, DO_QDMULH_H)
DO_2OP_SAT_SCALAR(vqdmulh_scalarw, 4, int32_t, DO_QDMULH_W)
DO_2OP_SAT_SCALAR(vqrdmulh_scalarb, 1, int8_t, DO_QRDMULH_B)
DO_2OP_SAT_SCALAR(vqrdmulh_scalarh, 2, int16_t, DO_QRDMULH_H)
DO_2OP_SAT_SCALAR(vqrdmulh_scalarw, 4, int32_t, DO_QRDMULH_W)

/*
 * Long saturating scalar ops. As with DO_2OP_L, TYPE and H are for the
 * input (smaller) type and LESIZE, LTYPE, LH for the output (long) type.
 * SATMASK specifies which bits of the predicate mask matter for determining
 * whether to propagate a saturation indication into FPSCR.QC -- for
 * the 16x16->32 case we must check only the bit corresponding to the T or B
 * half that we used, but for the 32x32->64 case we propagate if the mask
 * bit is set for either half.
 */
#define DO_2OP_SAT_SCALAR_L(OP, TOP, ESIZE, TYPE, LESIZE, LTYPE, FN, SATMASK) \
    void HELPER(glue(mve_, OP))(CPUARMState *env, void *vd, void *vn,   \
                                uint32_t rm)                            \
    {                                                                   \
        LTYPE *d = vd;                                                  \
        TYPE *n = vn;                                                   \
        TYPE m = rm;                                                    \
        uint16_t mask = mve_element_mask(env);                          \
        unsigned le;                                                    \
        bool qc = false;                                                \
        for (le = 0; le < 16 / LESIZE; le++, mask >>= LESIZE) {         \
            bool sat = false;                                           \
            LTYPE r = FN((LTYPE)n[H##ESIZE(le * 2 + TOP)], m, &sat);    \
            mergemask(&d[H##LESIZE(le)], r, mask);                      \
            qc |= sat && (mask & SATMASK);                              \
        }                                                               \
        if (qc) {                                                       \
            env->vfp.qc[0] = qc;                                        \
        }                                                               \
        mve_advance_vpt(env);                                           \
    }

static inline int32_t do_qdmullh(int16_t n, int16_t m, bool *sat)
{
    int64_t r = ((int64_t)n * m) * 2;
    return do_sat_bhw(r, INT32_MIN, INT32_MAX, sat);
}

static inline int64_t do_qdmullw(int32_t n, int32_t m, bool *sat)
{
    /* The multiply can't overflow, but the doubling might */
    int64_t r = (int64_t)n * m;
    if (r > INT64_MAX / 2) {
        *sat = true;
        return INT64_MAX;
    } else if (r < INT64_MIN / 2) {
        *sat = true;
        return INT64_MIN;
    } else {
        return r * 2;
    }
}

#define SATMASK16B 1
#define SATMASK16T (1 << 2)
#define SATMASK32 ((1 << 4) | 1)

DO_2OP_SAT_SCALAR_L(vqdmullb_scalarh, 0, 2, int16_t, 4, int32_t, \
                    do_qdmullh, SATMASK16B)
DO_2OP_SAT_SCALAR_L(vqdmullb_scalarw, 0, 4, int32_t, 8, int64_t, \
                    do_qdmullw, SATMASK32)
DO_2OP_SAT_SCALAR_L(vqdmullt_scalarh, 1, 2, int16_t, 4, int32_t, \
                    do_qdmullh, SATMASK16T)
DO_2OP_SAT_SCALAR_L(vqdmullt_scalarw, 1, 4, int32_t, 8, int64_t, \
                    do_qdmullw, SATMASK32)

/*
 * Long saturating ops
 */
#define DO_2OP_SAT_L(OP, TOP, ESIZE, TYPE, LESIZE, LTYPE, FN, SATMASK)  \
    void HELPER(glue(mve_, OP))(CPUARMState *env, void *vd, void *vn,   \
                                void *vm)                               \
    {                                                                   \
        LTYPE *d = vd;                                                  \
        TYPE *n = vn, *m = vm;                                          \
        uint16_t mask = mve_element_mask(env);                          \
        unsigned le;                                                    \
        bool qc = false;                                                \
        for (le = 0; le < 16 / LESIZE; le++, mask >>= LESIZE) {         \
            bool sat = false;                                           \
            LTYPE op1 = n[H##ESIZE(le * 2 + TOP)];                      \
            LTYPE op2 = m[H##ESIZE(le * 2 + TOP)];                      \
            mergemask(&d[H##LESIZE(le)], FN(op1, op2, &sat), mask);     \
            qc |= sat && (mask & SATMASK);                              \
        }                                                               \
        if (qc) {                                                       \
            env->vfp.qc[0] = qc;                                        \
        }                                                               \
        mve_advance_vpt(env);                                           \
    }

DO_2OP_SAT_L(vqdmullbh, 0, 2, int16_t, 4, int32_t, do_qdmullh, SATMASK16B)
DO_2OP_SAT_L(vqdmullbw, 0, 4, int32_t, 8, int64_t, do_qdmullw, SATMASK32)
DO_2OP_SAT_L(vqdmullth, 1, 2, int16_t, 4, int32_t, do_qdmullh, SATMASK16T)
DO_2OP_SAT_L(vqdmulltw, 1, 4, int32_t, 8, int64_t, do_qdmullw, SATMASK32)

static inline uint32_t do_vbrsrb(uint32_t n, uint32_t m)
{
    m &= 0xff;
    if (m == 0) {
        return 0;
    }
    n = revbit8(n);
    if (m < 8) {
        n >>= 8 - m;
    }
    return n;
}

static inline uint32_t do_vbrsrh(uint32_t n, uint32_t m)
{
    m &= 0xff;
    if (m == 0) {
        return 0;
    }
    n = revbit16(n);
    if (m < 16) {
        n >>= 16 - m;
    }
    return n;
}

static inline uint32_t do_vbrsrw(uint32_t n, uint32_t m)
{
    m &= 0xff;
    if (m == 0) {
        return 0;
    }
    n = revbit32(n);
    if (m < 32) {
        n >>= 32 - m;
    }
    return n;
}

DO_2OP_SCALAR(vbrsrb, 1, uint8_t, do_vbrsrb)
DO_2OP_SCALAR(vbrsrh, 2, uint16_t, do_vbrsrh)
DO_2OP_SCALAR(vbrsrw, 4, uint32_t, do_vbrsrw)

/*
 * Multiply add long dual accumulate ops.
 */
#define DO_LDAV(OP, ESIZE, TYPE, XCHG, EVENACC, ODDACC)                 \
    uint64_t HELPER(glue(mve_, OP))(CPUARMState *env, void *vn,         \
                                    void *vm, uint64_t a)               \
    {                                                                   \
        uint16_t mask = mve_element_mask(env);                          \
        unsigned e;                                                     \
        TYPE *n = vn, *m = vm;                                          \
        for (e = 0; e < 16 / ESIZE; e++, mask >>= ESIZE) {              \
            if (mask & 1) {                                             \
                if (e & 1) {                                            \
                    a ODDACC                                            \
                        (int64_t)n[H##ESIZE(e - 1 * XCHG)] * m[H##ESIZE(e)]; \
                } else {                                                \
                    a EVENACC                                           \
                        (int64_t)n[H##ESIZE(e + 1 * XCHG)] * m[H##ESIZE(e)]; \
                }                                                       \
            }                                                           \
        }                                                               \
        mve_advance_vpt(env);                                           \
        return a;                                                       \
    }

DO_LDAV(vmlaldavsh, 2, int16_t, false, +=, +=)
DO_LDAV(vmlaldavxsh, 2, int16_t, true, +=, +=)
DO_LDAV(vmlaldavsw, 4, int32_t, false, +=, +=)
DO_LDAV(vmlaldavxsw, 4, int32_t, true, +=, +=)

DO_LDAV(vmlaldavuh, 2, uint16_t, false, +=, +=)
DO_LDAV(vmlaldavuw, 4, uint32_t, false, +=, +=)

DO_LDAV(vmlsldavsh, 2, int16_t, false, +=, -=)
DO_LDAV(vmlsldavxsh, 2, int16_t, true, +=, -=)
DO_LDAV(vmlsldavsw, 4, int32_t, false, +=, -=)
DO_LDAV(vmlsldavxsw, 4, int32_t, true, +=, -=)

/*
 * Rounding multiply add long dual accumulate high: we must keep
 * a 72-bit internal accumulator value and return the top 64 bits.
 */
#define DO_LDAVH(OP, ESIZE, TYPE, XCHG, EVENACC, ODDACC, TO128)         \
    uint64_t HELPER(glue(mve_, OP))(CPUARMState *env, void *vn,         \
                                    void *vm, uint64_t a)               \
    {                                                                   \
        uint16_t mask = mve_element_mask(env);                          \
        unsigned e;                                                     \
        TYPE *n = vn, *m = vm;                                          \
        Int128 acc = int128_lshift(TO128(a), 8);                        \
        for (e = 0; e < 16 / ESIZE; e++, mask >>= ESIZE) {              \
            if (mask & 1) {                                             \
                if (e & 1) {                                            \
                    acc = ODDACC(acc, TO128(n[H##ESIZE(e - 1 * XCHG)] * \
                                            m[H##ESIZE(e)]));           \
                } else {                                                \
                    acc = EVENACC(acc, TO128(n[H##ESIZE(e + 1 * XCHG)] * \
                                             m[H##ESIZE(e)]));          \
                }                                                       \
                acc = int128_add(acc, int128_make64(1 << 7));           \
            }                                                           \
        }                                                               \
        mve_advance_vpt(env);                                           \
        return int128_getlo(int128_rshift(acc, 8));                     \
    }

DO_LDAVH(vrmlaldavhsw, 4, int32_t, false, int128_add, int128_add, int128_makes64)
DO_LDAVH(vrmlaldavhxsw, 4, int32_t, true, int128_add, int128_add, int128_makes64)

DO_LDAVH(vrmlaldavhuw, 4, uint32_t, false, int128_add, int128_add, int128_make64)

DO_LDAVH(vrmlsldavhsw, 4, int32_t, false, int128_add, int128_sub, int128_makes64)
DO_LDAVH(vrmlsldavhxsw, 4, int32_t, true, int128_add, int128_sub, int128_makes64)

/* Vector add across vector */
#define DO_VADDV(OP, ESIZE, TYPE)                               \
    uint32_t HELPER(glue(mve_, OP))(CPUARMState *env, void *vm, \
                                    uint32_t ra)                \
    {                                                           \
        uint16_t mask = mve_element_mask(env);                  \
        unsigned e;                                             \
        TYPE *m = vm;                                           \
        for (e = 0; e < 16 / ESIZE; e++, mask >>= ESIZE) {      \
            if (mask & 1) {                                     \
                ra += m[H##ESIZE(e)];                           \
            }                                                   \
        }                                                       \
        mve_advance_vpt(env);                                   \
        return ra;                                              \
    }                                                           \

DO_VADDV(vaddvsb, 1, uint8_t)
DO_VADDV(vaddvsh, 2, uint16_t)
DO_VADDV(vaddvsw, 4, uint32_t)
DO_VADDV(vaddvub, 1, uint8_t)
DO_VADDV(vaddvuh, 2, uint16_t)
DO_VADDV(vaddvuw, 4, uint32_t)
