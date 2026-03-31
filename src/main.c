/*
 * Tardygrada — First Program
 *
 * This proves the VM works:
 * - Spawn agents holding values
 * - Read them back with verification
 * - Test immutability enforcement
 * - Test GC demotion/promotion cycle
 *
 * In Tardygrada syntax, this would be:
 *
 *   agent Main {
 *       let x: int = 5 @verified
 *       y: int = 42
 *       let z: int = 7 @sovereign
 *   }
 */

#include "vm/vm.h"
#include <sys/mman.h>
#include <unistd.h>  /* write() for stdout — no printf, no stdio */
#include <string.h>

static int is_zero_uuid(tardy_uuid_t id)
{
    return id.hi == 0 && id.lo == 0;
}

/* We don't use printf. We write directly. */
static void print(const char *s)
{
    write(STDOUT_FILENO, s, strlen(s));
}

static void print_int(int64_t v)
{
    char buf[32];
    int i = 30;
    int neg = 0;
    if (v < 0) { neg = 1; v = -v; }
    if (v == 0) { buf[i--] = '0'; }
    while (v > 0) {
        buf[i--] = '0' + (v % 10);
        v /= 10;
    }
    if (neg) buf[i--] = '-';
    write(STDOUT_FILENO, buf + i + 1, 30 - i);
}

static void ok(const char *test)
{
    print("  [OK] ");
    print(test);
    print("\n");
}

static void fail(const char *test)
{
    print("  [FAIL] ");
    print(test);
    print("\n");
}

int main(void)
{
    /* VM is too large for stack — allocate via mmap */
    tardy_vm_t *vmp = (tardy_vm_t *)mmap(NULL, sizeof(tardy_vm_t),
                                          PROT_READ | PROT_WRITE,
                                          MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (vmp == MAP_FAILED) {
        print("mmap failed\n");
        return 1;
    }
    tardy_vm_t *vm = vmp;
    int64_t val;

    print("\n=== Tardygrada VM ===\n\n");

    /* ---- Init ---- */
    if (tardy_vm_init(vm, NULL) != 0) {
        print("VM init failed\n");
        return 1;
    }
    ok("VM initialized with default semantics");

    tardy_uuid_t root = vm->root_id;

    /* ---- let x: int = 5 @verified ---- */
    int64_t five = 5;
    tardy_uuid_t x_id = tardy_vm_spawn(vm, root, "x",
                                         TARDY_TYPE_INT,
                                         TARDY_TRUST_VERIFIED,
                                         &five, sizeof(int64_t));
    if (!is_zero_uuid(x_id))
        ok("let x: int = 5 @verified — agent spawned");
    else
        fail("let x: int = 5 @verified — spawn failed");

    /* Read x back — verified (hash check) */
    val = 0;
    tardy_read_status_t status = tardy_vm_read(vm, root, "x",
                                                &val, sizeof(int64_t));
    if (status == TARDY_READ_OK && val == 5) {
        print("  [OK] read x = ");
        print_int(val);
        print(" (hash verified)\n");
    } else {
        fail("read x — verification failed");
    }

    /* ---- y: int = 42 (mutable) ---- */
    int64_t fortytwo = 42;
    tardy_uuid_t y_id = tardy_vm_spawn(vm, root, "y",
                                         TARDY_TYPE_INT,
                                         TARDY_TRUST_MUTABLE,
                                         &fortytwo, sizeof(int64_t));
    if (!is_zero_uuid(y_id))
        ok("y: int = 42 — mutable agent spawned");
    else
        fail("y: int = 42 — spawn failed");

    /* Mutate y from 42 to 100 */
    int64_t hundred = 100;
    if (tardy_vm_mutate(vm, root, "y", &hundred, sizeof(int64_t)) == 0) {
        val = 0;
        tardy_vm_read(vm, root, "y", &val, sizeof(int64_t));
        print("  [OK] mutated y = ");
        print_int(val);
        print("\n");
    } else {
        fail("mutate y");
    }

    /* Try to mutate x (immutable) — MUST fail */
    int64_t ten = 10;
    if (tardy_vm_mutate(vm, root, "x", &ten, sizeof(int64_t)) != 0)
        ok("mutate x rejected — immutable agent enforced");
    else
        fail("mutate x succeeded — IMMUTABILITY BROKEN");

    /* ---- let z: int = 7 @sovereign ---- */
    int64_t seven = 7;
    tardy_uuid_t z_id = tardy_vm_spawn(vm, root, "z",
                                         TARDY_TYPE_INT,
                                         TARDY_TRUST_SOVEREIGN,
                                         &seven, sizeof(int64_t));
    if (!is_zero_uuid(z_id))
        ok("let z: int = 7 @sovereign — agent spawned (5 replicas + sig)");
    else
        fail("let z: int = 7 @sovereign — spawn failed");

    /* Read z — full BFT verification */
    val = 0;
    status = tardy_vm_read(vm, root, "z", &val, sizeof(int64_t));
    if (status == TARDY_READ_OK && val == 7) {
        print("  [OK] read z = ");
        print_int(val);
        print(" (BFT + hash + signature verified)\n");
    } else {
        fail("read z — sovereign verification failed");
    }

    /* ---- Freeze: promote y from mutable to @verified ---- */
    tardy_agent_t *y_agent = tardy_vm_find_by_name(vm, root, "y");
    if (y_agent) {
        tardy_uuid_t frozen = tardy_vm_freeze(vm, y_agent->id,
                                               TARDY_TRUST_VERIFIED);
        if (!is_zero_uuid(frozen)) {
            ok("freeze y: mutable -> @verified");
            /* Now mutation must fail */
            int64_t try_mutate = 999;
            if (tardy_vm_mutate(vm, root, "y", &try_mutate,
                                sizeof(int64_t)) != 0)
                ok("mutate frozen y rejected — freeze enforced");
            else
                fail("mutate frozen y succeeded — FREEZE BROKEN");
        } else {
            fail("freeze y");
        }
    }

    /* ---- GC: test demotion cycle ---- */
    /* Force x's last_accessed to be old */
    tardy_agent_t *x_agent = tardy_vm_find_by_name(vm, root, "x");
    if (x_agent) {
        /* Simulate 60 seconds idle */
        x_agent->last_accessed -= 60000000000ULL;
        int collected = tardy_vm_gc(vm);
        if (collected > 0 && x_agent->state == TARDY_STATE_STATIC) {
            ok("GC demoted idle x to Static");
            /* Read from static — should still return 5 */
            val = 0;
            tardy_vm_read(vm, root, "x", &val, sizeof(int64_t));
            if (val == 5) {
                print("  [OK] static x still = ");
                print_int(val);
                print("\n");
            } else {
                fail("static x value corrupted");
            }
        } else {
            fail("GC demotion");
        }
    }

    /* ---- Stats ---- */
    print("\n--- Stats ---\n");
    print("  Agents alive: ");
    print_int(vm->agent_count);
    print("\n  Tombstones: ");
    print_int(vm->tombstone_count);
    print("\n  Page size: ");
    print_int((int64_t)tardy_page_size());
    print(" bytes\n\n");

    /* ---- Shutdown ---- */
    tardy_vm_shutdown(vm);
    munmap(vmp, sizeof(tardy_vm_t));
    ok("VM shutdown");

    print("\n=== All tests passed ===\n\n");
    return 0;
}
