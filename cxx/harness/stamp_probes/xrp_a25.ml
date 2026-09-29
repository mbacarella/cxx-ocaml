module P = struct
  module type A = sig type t type u end
  module MyMap(X : A with type t := int) = X
end
