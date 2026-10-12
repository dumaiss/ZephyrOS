# ZephyrOS helloworld demos (work/28c-m9-user-sdk).
#
# The top-level Makefile includes this fragment, so every demo is declared
# here with one zex_program line from user/sdk/zex.mk:
#
#   make build/user/hello.zex     build the demo in the production tree
#   make helloworld-image         boot it as /system/hello.zex
#
# STAGE=1 copies the packed program into $(MODULES) so the selected
# recovery manifest can name it.  Add a demo by dropping a .c file next to
# hello.c and adding its own zex_program line (docs/sdk.md).

$(eval $(call zex_program,hello,user/helloworld,hello.c,STACK=256,STAGE=1))

# Far-code demo (work package 44b): fardemo_far.c is compiled into the
# _FAR_CODE area and linked at 8000h; the rule declares one FAR_CODE
# segment for it.
$(eval $(call zex_program,fardemo,user/helloworld,fardemo.c,\
	FAR=fardemo_far.c,STACK=256,STAGE=1))
