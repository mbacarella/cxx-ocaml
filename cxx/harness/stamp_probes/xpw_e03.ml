module X = struct module Y = struct module type S = sig type t end end end
module Y = X.Y
module M = struct type t = int end
let m = (module M : Y.S with type t = int)
