module type S = sig
  module M : sig val s : unit end
 end
let x = (module struct
    module M = struct let s = () end
  end : S)
module X = (val x)
module Y = X.M
