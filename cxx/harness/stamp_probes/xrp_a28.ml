module P = struct
  module type A = sig type t end
  module MyMap(X : A) = (X : A)
end
