#pragma once
// An include file with a type error on line 5: a vec3 returned as vec4.

vec4 brokenColor() {
    return vec3(1.0);
}
