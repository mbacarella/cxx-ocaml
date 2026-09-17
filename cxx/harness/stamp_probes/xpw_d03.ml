module X = struct module type S = sig type t module M : sig type s end end end
module M = struct type t = int module M = struct type s end end
let m = (module M : X.S with type t = int)
