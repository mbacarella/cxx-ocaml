module P = struct
  module type A = sig type t end
  module MyMap(X : A) = struct type t = X.t end
end
