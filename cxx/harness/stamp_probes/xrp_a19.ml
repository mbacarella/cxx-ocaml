module P = struct
  module type MyT = sig type t module M : sig type u val v : u end end
  module MyMap(X : MyT) = X
end
