#!/bin/bash
arch="arm"
KERNEL_DIR=$PWD
export KBUILD_BUILD_HOST="eOS-0.4.1-Loki"
export KBUILD_BUILD_USER="arttttt"
export ARCH=$arch
clean_build=0
config="tegra12_android_defconfig"
original_output="$KERNEL_DIR/arch/arm/boot"
output="$KERNEL_DIR/output"
output_archive="SmokeR24.2"
threads=5
toolchain="$HOME/Android/toolchain/linaro-4.9.4/bin/arm-linux-gnueabihf-"

make_zip()
{
	printf "\nСоздание zip архива\n\n"

	cd $KERNEL_DIR/anykernel
	zip_name="$output_archive($(date +'%d.%m.%Y-%H:%M'))"
	zip -r $zip_name.zip *

	if [ -f "$PWD/$zip_name.zip" ]; then
		printf "\n$zip_name.zip создан, перемещение в $output"
		mv "$PWD/$zip_name.zip" $output

		if [ -f "$output/$zip_name.zip" ]; then
			echo
			printf "\nЗавершено\n"
		fi
	else
		printf "\nНе удалось создать архив\n"
		exit 0
	fi
}

compile()
{
	start=$(date +%s)
	clear

	if [ "$clean_build" == 1 ]; then
		make clean
		make mrproper
	fi

	make $config
	make -j$threads ARCH=$arch CROSS_COMPILE=$toolchain zImage

	if [ -f "$original_output/zImage" ]; then
		mv $original_output/zImage $PWD/anykernel/kernel/
	else
		exit 0
	fi

	printf "\nКомпиляция дерева устройства\n\n"

	make -j$threads ARCH=$arch CROSS_COMPILE=$toolchain tegra124-mocha.dtb

	if [ -f "$original_output/dts/tegra124-mocha.dtb" ]; then
		mv $original_output/dts/tegra124-mocha.dtb $PWD/anykernel/kernel/dtb
	else
		exit 0
	fi

	end=$(date +%s)
	comp_time=$((end-start))
	printf "\nЯдро скомпилировано за %02d:%02d\n" $((($comp_time/60)%60)) $(($comp_time%60))

	make_zip
}

compile_dtb()
{
	clear
	make $config
	make -j$threads ARCH=$arch CROSS_COMPILE=$toolchain tegra124-mocha.dtb

	if [ -f "$original_output/dts/tegra124-mocha.dtb" ]; then
		mv $original_output/dts/tegra124-mocha.dtb $PWD/anykernel/kernel/dtb
	else
		exit 0
	fi

	make_zip
}

main()
{
	clear
	echo "---------------------------------------------------"
	echo "Выполнить чистую сборку?                          -"
	echo "---------------------------------------------------"
	echo "1 - Да                                            -"
	echo "---------------------------------------------------"
	echo "2 - Нет                                           -"
	echo "---------------------------------------------------"
	echo "3 - Собрать dtb                                   -"
	echo "---------------------------------------------------"
	echo "4 - Выход                                         -"
	echo "---------------------------------------------------"
	printf %s "Ваш выбор: "
	read env

	case $env in
		1) clean_build=1;compile;;
		2) compile;;
		3) compile_dtb;;
		4) exit 0;;
		*) main;;
	esac
}

main
