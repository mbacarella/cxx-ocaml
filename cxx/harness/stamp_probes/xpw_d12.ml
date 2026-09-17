module type S = sig type t module M : sig type s end end
module M = struct type t = int module M = struct type s end end
let f (module M : S with type t = int) = ()
