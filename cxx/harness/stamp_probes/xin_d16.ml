module A = struct module type S = sig type t end
module Make (M : sig end) = struct let f s = s end include Make (struct end)
end
let x = A.f 1
