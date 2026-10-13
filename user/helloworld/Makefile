# KonaOS helloworld demo image.
#
# The top-level Makefile includes this fragment, so the demos are
# declared here with one exe_program line from the SDK rules.  Their
# sources live with the SDK's examples (user/sdk/examples): the demo
# image boots the very programs the standalone SDK ships, so a broken
# example fails this image too.
#
#   make build/user/hello.exe     build the demo in the production tree
#   make helloworld-image         boot it as /system/hello.exe
#
# STAGE=1 copies the packed program into $(MODULES) so the selected
# recovery manifest can name it.  Add a demo by dropping a .c file into
# user/sdk/examples and adding its own exe_program line (docs/sdk.md).

$(eval $(call exe_program,hello,user/sdk/examples,hello.c,STACK=256,STAGE=1))

# Far-code demo: fardemo_far.c is compiled into
# the _FAR1 area and linked at 8000h; the rule declares one FAR_CODE
# segment for it.
$(eval $(call exe_program,fardemo,user/sdk/examples,fardemo.c,\
	FAR1=fardemo_far.c,STACK=256,STAGE=1))
