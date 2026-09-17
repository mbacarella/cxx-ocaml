module P = struct
  module type A = sig type t end
  module type B = sig type u end
  module MyMap(X : A) (Y : B) = X
end
