module type S = sig type t module M : sig type s end end
type u = (module S with type t = int)
