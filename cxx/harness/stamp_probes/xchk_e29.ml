module type S = sig
  module M : sig val s : unit end
  module F : sig val f : unit end
 end
let x = (module struct
    module M = struct let s = () end
    module F = struct let f = () end
  end : S)
module X = (val x)
module Y = X.M
let _ = Y.s
