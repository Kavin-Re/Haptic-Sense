#!/bin/bash

# Generate the Haptic-Sense hazard classifier for the STM32N6 NPU (Block 8).
# NOTE (2026-09-17): this points at the real trained model now
# (haptic_sense_hazard_v4.tflite, 17->32->16->1 int8 MLP) instead of the
# ST Person-Detection YOLOX placeholder this project was scaffolded from.
# user_neuralart.json's --mvei/--native-float/etc. options and the
# my_mpools/stm32n6-app2.mpool memory pool were tuned for a 480x480 image
# model and have NOT been re-tuned for this much smaller dense network --
# review both before relying on stedgeai's output.
# UPDATE (2026-09-25): the committed firmware/Model/STM32N6570-DK/ artifacts
# (network.c/.h, network_ecblobs.h, network_data.hex at 0x71000000) were
# generated from this model with ST Edge AI Core 4.0.1, and the NPU path
# (HAZARD_CLASSIFIER_USE_NPU) is the shipped build. The CPU path
# (app_hazard_classifier.c, bit-exact against this .tflite) stays in the
# source as the alternative build. Outputs land in the current directory;
# copy them into firmware/Model/STM32N6570-DK/ by hand.
stedgeai generate --no-inputs-allocation --no-outputs-allocation --model haptic_sense_hazard_v4.tflite --target stm32n6 --st-neural-art default@user_neuralart.json
cp st_ai_output/network_ecblobs.h .
cp st_ai_output/network.c .
cp st_ai_output/network.h .
cp st_ai_output/network_atonbuf.xSPI2.raw network_data.xSPI2.bin
arm-none-eabi-objcopy -I binary network_data.xSPI2.bin --change-addresses 0x71000000 -O ihex network_data.hex