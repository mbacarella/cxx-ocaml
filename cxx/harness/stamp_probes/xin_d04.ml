module A = struct module Make (M : sig end) = struct type t = int end
include Make (struct end) end
type u = A.t
