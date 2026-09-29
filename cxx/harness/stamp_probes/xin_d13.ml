module A = struct module Make (M : sig end) = struct let f s = s end
module M0 = struct end include Make (M0) end
let x = A.f 1
