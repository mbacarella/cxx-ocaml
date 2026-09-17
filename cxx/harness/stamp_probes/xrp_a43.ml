module P = struct
  module type MyT = sig module M : sig type u end end
  module MyMap(X : MyT) = X
end
