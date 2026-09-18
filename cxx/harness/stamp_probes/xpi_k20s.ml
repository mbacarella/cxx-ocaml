module A = struct module M = struct type t = int end end
module type S = sig type t end
let _ = (module A.M : S)
