#version 450
layout(location=0) flat in uint value;
layout(location=0) out uint color;
layout(location=1) out uint other;
void main() { color=value; other=value+200u; }
