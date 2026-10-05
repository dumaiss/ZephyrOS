/* ZephyrOS M9 SYS_EXEC raw-return child (test artifact, package 28).
 *
 * rw_main returns without calling SYS_EXIT.  The entry stub then returns
 * through the common trap, so the kernel tears the child down with
 * ZOS_EFAULT; the parent's zos_exec() call must observe that exact status.
 */

void rw_main(void)
{
}
