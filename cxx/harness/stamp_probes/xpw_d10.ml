module type S = sig type t module M : sig type s end end
let f x = (x : (module S with type t = int))
