import argparse
import os
import re

from alt_wkl_configs import *
import ast
from get_output_sizes import calc_conv_output_size, calc_maxpool_output_size

broken_networks = []
broken_files = []


def check_layer_count_match(len_data_records, layer_count_line):
    match = re.search(r"Layer count:: (\d+)", layer_count_line)
    if match:
        layer_count_number = int(match.group(1))
        if layer_count_number == len_data_records:
            return True
        else:
            return False
    else:
        return False


def file_write_line_conv(cur_conv, layer_type):
    conv_out_height, conv_out_width, out_vol = calc_conv_output_size(cur_conv)

    ofm_dim_label = (conv_out_height, conv_out_width, cur_conv.out_filter)

    ifm_dim_label = (cur_conv.height, cur_conv.width, cur_conv.in_filter)

    kernel_dim_label = (cur_conv.hkernel, cur_conv.wkernel)

    stride_label = (cur_conv.hstride, cur_conv.wstride)

    pad_label = (cur_conv.hpad, cur_conv.wpad)

    return '\t'.join([layer_type, str(ifm_dim_label), str(ofm_dim_label), str(out_vol), str(kernel_dim_label),
                      str(stride_label), str(pad_label)]) + '\n'


def file_write_line_maxpool(cur_conv_cfg, maxpool_cfg, layer_type):
    conv_out_height, conv_out_width, _ = calc_conv_output_size(cur_conv_cfg)

    ifm_dim_label = (conv_out_height, conv_out_width, cur_conv_cfg.out_filter)

    mp_out_height, mp_out_width, out_vol = calc_maxpool_output_size(cur_conv_cfg, maxpool_cfg)

    ofm_dim_label = (mp_out_height, mp_out_height, cur_conv_cfg.out_filter)

    kernel_dim_label = (maxpool_cfg.hkernel, maxpool_cfg.wkernel)

    stride_label = (maxpool_cfg.hstride, maxpool_cfg.wstride)

    pad_label = (maxpool_cfg.hpad, maxpool_cfg.wpad)

    return '\t'.join([layer_type, str(ifm_dim_label), str(ofm_dim_label), str(out_vol), str(kernel_dim_label),
                      str(stride_label), str(pad_label)]) + '\n'


def generate_labels_file(networks, output_dataset_dir):
    for network in networks:
        network_name, layers = network.split("::")
        with open(os.path.join(output_dataset_dir, network_name + ".txt"), 'w+') as myfile:
            myfile.write(
                '\t'.join(["layer_type", "ifm_dim", "ofm_dim", "output_vol", "kernel_dim", "stride", "pad"]) + '\n')

        layers_nt = []
        layers = layers.split("), ")
        for layer in layers:
            layer = layer.replace("[", "")
            layer = layer.replace("]", "")
            layer = layer.replace("\n", "")
            if ')' not in layer:
                layer = layer + ")"
            layer_wkl = eval(layer)
            layers_nt.append(layer_wkl)

        layer_type = []
        cur_conv = None
        for i, layer in enumerate(layers_nt):

            if isinstance(layer, Conv2DWorkload):
                if i > 0 and len(layer_type) > 0:
                    with open(os.path.join(output_dataset_dir, network_name + ".txt"), 'a') as myfile:
                        myfile.write(file_write_line_conv(cur_conv, "".join(layer_type)))
                layer_type = []
                cur_conv = layer
                layer_type.append('C')
            elif isinstance(layer, BatchNorm2DConfig):
                layer_type.append('B')
            elif isinstance(layer, ReluConfig):
                layer_type.append('R')
            elif isinstance(layer, MaxPool2DConfig):
                if i > 0 and len(layer_type) > 0:
                    with open(os.path.join(output_dataset_dir, network_name + ".txt"), 'a') as myfile:
                        myfile.write(file_write_line_conv(cur_conv, "".join(layer_type)))

                layer_type = ['M']
                with open(os.path.join(output_dataset_dir, network_name + ".txt"), 'a') as myfile:
                    myfile.write(file_write_line_maxpool(cur_conv, layer, "".join(layer_type)))

                layer_type = []

        if len(layer_type) > 0:
            with open(os.path.join(output_dataset_dir, network_name + ".txt"), 'a') as myfile:
                myfile.write(file_write_line_conv(cur_conv, "".join(layer_type)))


def clean_data_records(log_files_dir, output_dataset_dir):
    for i, filename in enumerate(os.listdir(output_dataset_dir)):
        if 'network' not in filename:
            continue
        with open(os.path.join(output_dataset_dir, filename)) as dataset_file:
            filename_no_ext = filename.split('.')[0]
            dataset_file_lines = dataset_file.readlines()[1:]
            with open(os.path.join(log_files_dir, filename_no_ext + '_sample0.log'), 'r') as data_record_file:
                data_record_file_lines = data_record_file.readlines()
                layer_count_line = data_record_file_lines[-1]
                data_record_file_lines = data_record_file_lines[:-1]
            len_dataset = len(dataset_file_lines)
            len_data_record = len(data_record_file_lines)
            if len_data_record == 0:
                broken_files.append(filename_no_ext + '_sample0.log')
                break
            if not check_layer_count_match(len_data_record, layer_count_line):
                new_layer_count_line = f'Layer count:: {len_data_record}'
                lines = data_record_file_lines
                lines.append(new_layer_count_line)
                with open(os.path.join(log_files_dir, filename_no_ext + '_sample0.log'), 'w+') as data_record_file:
                    data_record_file.writelines(lines)

            if len_dataset == len_data_record:
                continue
            else:
                for i, line in enumerate(dataset_file_lines):
                    if i >= len(data_record_file_lines):
                        broken_networks.append(filename_no_ext)
                        break
                    req_total = int(line.split('\t')[3])
                    cur_total = int(data_record_file_lines[i].split(':')[1])
                    if cur_total == req_total:
                        continue
                    elif 2 * req_total == cur_total:
                        double_line = data_record_file_lines[i]
                        half_line = ":".join([str(int(int(reading) / 2)) for reading in double_line.split(':')]) + '\n'
                        data_record_file_lines[i] = half_line
                        data_record_file_lines.insert(i + 1, half_line)
                        cur_total = int(half_line.split(':')[1])
                    elif cur_total < req_total:
                        cur_line = i
                        while cur_total < req_total:
                            cur_line += 1
                            if cur_line >= len_data_record:
                                break
                            try:
                                cur_total += int(data_record_file_lines[cur_line].split(':')[1])
                            except:
                                print(
                                    f'Exception occurred at network {filename_no_ext}. putting it as a broken network')
                                broken_networks.append(filename_no_ext)
                                break
                        if cur_total != req_total:
                            broken_networks.append(filename_no_ext)
                            break
                        lines_to_combine = data_record_file_lines[i: cur_line + 1]
                        combined_line = "0:0:0:0:0"
                        for ln in lines_to_combine:
                            combined_line = ':'.join(
                                [str(int(combined_line.split(':')[j]) + int(cur_val)) for j, cur_val in
                                 enumerate(ln.split(':'))])
                        combined_line = combined_line + '\n'
                        data_record_file_lines[i] = combined_line
                        del data_record_file_lines[i + 1:cur_line + 1]
                    if cur_total != req_total:
                        broken_networks.append(filename_no_ext)
                        break
                if not check_layer_count_match(len(data_record_file_lines), layer_count_line):
                    new_layer_count_line = f'Layer count:: {len(data_record_file_lines)}'
                    data_record_file_lines.append(new_layer_count_line)
                    with open(os.path.join(log_files_dir, filename_no_ext + '_sample0.log'), 'w+') as data_record_file:
                        data_record_file.writelines(data_record_file_lines)
                else:
                    data_record_file_lines.append(layer_count_line)
                    with open(os.path.join(log_files_dir, filename_no_ext + '_sample0.log'), 'w+') as data_record_file:
                        data_record_file.writelines(data_record_file_lines)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(
        description='UART Sniffer random compute graphs dataset preparation script')
    parser.add_argument('--log_files_dir', type=str, default="uart_sniffer_data/asp_dac/rcg/4x8x8",
                        help='apm log files dir')
    parser.add_argument('--networks_file', type=str,
                        default="profiling_results/uart_sniffer/asp_dac/rcg/4x8x8/networks_profiled.log",
                        help='profiled networks list')
    parser.add_argument('--output_dataset_dir', type=str, default="dataset/uart_sniffer/asp_dac/rcg/4x8x8",
                        help='output dataset directory')

    args = parser.parse_args()

    log_files_dir = args.log_files_dir

    networks_file = args.networks_file

    with open(networks_file, 'r') as myfile:
        networks = myfile.readlines()
        generate_labels_file(networks, args.output_dataset_dir)

    # clean_data_records(args.log_files_dir, args.output_dataset_dir)
    # broken_networks = list(set(broken_networks))
    # print("Broken networks:: ", broken_networks)
    # print("Broken files:: ", broken_files)