module X = struct module type S = sig type t end end
module M = struct type t = int end
let m = (module M : X.S with type t = int)
type u = (module X.S with type t = unit)
