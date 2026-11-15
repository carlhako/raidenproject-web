# Browser build: make web   (needs emsdk: source ~/tools/emsdk/emsdk_env.sh)
EMCC     ?= emcc
WEB_GEN   = $(foreach p,$(PROGS),$(shell cat gen/$(p).files 2>/dev/null))
WEB_SRC   = $(filter-out runtime/platform.c,$(wildcard runtime/*.c))
WEB_OBJ   = $(WEB_SRC:%.c=build/web/%.o) $(WEB_GEN:%.c=build/web/%.o)
WEB_CFLAGS = -O2 -g0 -Iruntime -fno-strict-aliasing -fwrapv -mtail-call -fwasm-exceptions -Wall \
  -Wno-unused-variable -Wno-misleading-indentation -Wno-unused-label -Wno-unused-but-set-variable -Wno-unused-function
WEB_LDFLAGS = -fwasm-exceptions -sSUPPORT_LONGJMP=wasm -sENVIRONMENT=worker,node -sINITIAL_MEMORY=64MB \
  -sSTACK_SIZE=16MB -sALLOW_MEMORY_GROWTH=1 -sMODULARIZE=1 -sEXPORT_NAME=createPsx -sINVOKE_RUN=0 \
  -sEXIT_RUNTIME=0 -sEXPORTED_RUNTIME_METHODS=callMain,HEAPU8,FS -sEXPORTED_FUNCTIONS=_main,_psx_state_hash \
  -sFILESYSTEM=1 --js-library web/psx-lib.js --embed-file extracted/JJ3X/PSJJ3.EXE@/extracted/PSJJ3.EXE

build/web/%.o: %.c runtime/recomp.h runtime/psx.h
	@mkdir -p $(dir $@)
	$(EMCC) $(WEB_CFLAGS) -c $< -o $@

dist/raiden.js: $(GENSTAMP) $(WEB_OBJ) web/psx-lib.js
	@mkdir -p dist
	$(EMCC) $(WEB_OBJ) -o dist/raiden.js $(WEB_LDFLAGS)

web: dist/raiden.js
	cp web/index.html web/style.css web/app.js web/display.js web/game-worker.js web/audio-worklet.js dist/
.PHONY: web
