import math
import numpy
import os

DATA_DIR = './qordered_attention'

def create_qordered_longformer_attention_graph():
    from onnx import helper, numpy_helper, TensorProto

    nodes = [
        helper.make_node('QuantizeWithOrder', inputs=['input', 'scale_input'], outputs=['input_s8_COL32'], name='1_QuantizeWithOrder', domain='com.microsoft', order_input=1, order_output=2),    
        helper.make_node('QuantizeWithOrder', inputs=['weight', 'scale_weight'], outputs=['weight_s8_COL4_4R2_8C_T'], name='2_QuantizeWithOrder', domain='com.microsoft', order_input=1, order_output=3),
        helper.make_node('QuantizeWithOrder', inputs=['global_weight', 'scale_global_weight'], outputs=['global_weight_s8_COL4_4R2_8C_T'], name='3_QuantizeWithOrder', domain='com.microsoft', order_input=1, order_output=3),
 
        helper.make_node(
            'QOrderedLongformerAttention',
            inputs=['input_s8_COL32', 'scale_input', 'weight_s8_COL4_4R2_8C_T', 'scale_weight', 
                    'bias', 'scale_bias', 'scale_qkv_gemm', 'mask', 
                    'global_weight_s8_COL4_4R2_8C_T', 'scale_global_weight', 'global_bias', 
                    'scale_global_gemm', 'global', 'scale_output'],
            outputs=['output_s8_COL32'],
            name='LongFormerAttention_quantized',
            domain='com.microsoft',
            num_heads=12,
            window=256,
            order_input=2,
            order_output=2,
            order_weight=3,
            order_global_weight=3
        ),
        
        helper.make_node('DequantizeWithOrder', inputs=['output_s8_COL32', 'scale_output'], outputs=['output'], name='1_DequantizeWithOrder', domain='com.microsoft', order_input=1, order_output=3),

    ]

    initializers = [
        numpy_helper.from_array(numpy.load(os.path.join(DATA_DIR, 'const64_764.npy')).astype('float32').reshape([768, 2304]), name='weight'),
        numpy_helper.from_array(numpy.load(os.path.join(DATA_DIR, 'const65_769.npy')).astype('float32').reshape([2304]), name='bias'),
        numpy_helper.from_array(numpy.load(os.path.join(DATA_DIR, 'const64_764.npy')).astype('float32').reshape([768, 2304]), name='global_weight'),
        numpy_helper.from_array(numpy.load(os.path.join(DATA_DIR, 'const65_769.npy')).astype('float32').reshape([2304]), name='global_bias'),
        numpy_helper.from_array(numpy.array(2, dtype='float32'), name='scale_input'),
        numpy_helper.from_array(numpy.array(0.007874015718698502, dtype='float32'), name='scale_weight'),
        numpy_helper.from_array(numpy.array(0.007874015718698502, dtype='float32'), name='scale_global_weight'),        
        numpy_helper.from_array(numpy.array(0.007874015718698502, dtype='float32'), name='scale_bias'),
        numpy_helper.from_array(numpy.array(0.007874015718698502, dtype='float32'), name='scale_qkv_gemm'),
        numpy_helper.from_array(numpy.array(0.007874015718698502, dtype='float32'), name='scale_global_gemm'),
        numpy_helper.from_array(numpy.array(0.007874015718698502, dtype='float32'), name='scale_output'),      
        numpy_helper.from_array(numpy.zeros((1, 32), dtype='float16'), name='mask'),
        numpy_helper.from_array(numpy.ones((1, 32), dtype='int32'), name='global'),
    ]

    graph = helper.make_graph(nodes, "QOrderedLongformerAttention_Graph", [
        helper.make_tensor_value_info('input', TensorProto.FLOAT, [1, 32, 768])
    ], [
        helper.make_tensor_value_info('output', TensorProto.FLOAT, [1, 32, 768]),
    ], initializers)

    model = helper.make_model(graph=graph)
    return model.SerializeToString()


onnx_model_str = create_qordered_longformer_attention_graph()

from onnxruntime import SessionOptions, InferenceSession
sess_options = SessionOptions()
ort_session = InferenceSession(onnx_model_str, sess_options, providers=['CUDAExecutionProvider'])

ort_inputs = {
    'input' : numpy.random.randint(-256, 254, [2, 32, 768]).astype('float32')
}

ort_output = ort_session.run(None, ort_inputs)
