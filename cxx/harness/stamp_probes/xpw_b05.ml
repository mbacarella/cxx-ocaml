module X = struct module type S = sig type t end end
module M = struct type t = int end
type t = (module X.S with type t = unit)
let m = (module M : X.S with type t = int)
