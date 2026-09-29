module type S = sig type t type t2 module M : sig type s end end
type u = (module S with type t = int and type t2 = int)
