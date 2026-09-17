module A = struct module Make (M : sig end) = struct let f s = s end
module N = Make (struct end) end
let x = A.N.f 1
