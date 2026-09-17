module type S = sig
  module M : sig val s : unit val r : unit end
 end
let x = (module struct
    module M = struct let s = () let r = () end
  end : S)
module X = (val x)
let _ = X.M.s
