module type S0 = sig type key end
module Outer = struct
  module type T = sig type t end
  module X : T = struct type t = int end
  module FX (Y : S0) = X
end
module FOX (Y : S0) = Outer.X
module FH (Y : S0) = Hashtbl
