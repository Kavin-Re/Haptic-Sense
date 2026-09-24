#!/bin/bash

# Generate the Haptic-Sense hazard classifier for the STM32N6 NPU (Block 8).
# NOTE (2026-09-17): this points at the real trained model now
# (haptic_sense_hazard_v4.tflite, 17->32->16->1 int8 MLP) instead of the
# ST Person-Detection YOLOX placeholder this project was scaffolded from.
# user_neuralart.json's --mvei/--native-float/etc. options and the
# my_mpools/stm32n6-app2.mpool memory pool were tuned for a 480x480 image
# model and have NOT been re-tuned for this much smaller dense network --
# review both before relying on stedgeai's output. stedgeai itself is not
# installed in this environment; this script has not been run yet.
# CPU-only fallback (app_hazard_classifier.c, validated bit-exact against
# this same .tflite) already ships and does not depend on this script.
stedgeai generate --no-inputs-allocation --no-outputs-allocation --model haptic_sense_hazard_v4.tflite --target stm32n6 --st-neural-art default@user_neuralart.json
cp st_ai_output/network_ecblobs.h .
cp st_ai_output/network.c .
cp st_ai_output/network.h .
cp st_ai_output/network_atonbuf.xSPI2.raw network_data.xSPI2.bin
arm-none-eabi-objcopy -I binary network_data.xSPI2.bin --change-addresses 0x71000000 -O ihex network_data.hex