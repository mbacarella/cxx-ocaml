module A = struct module N = struct let f s = s end include N end
let x = A.f 1
