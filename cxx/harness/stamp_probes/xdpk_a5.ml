module N = struct
  module O = struct module type T = sig val w : string end end end
let f (module X : N.O.T) = X.w
