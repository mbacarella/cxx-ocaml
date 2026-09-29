module P = struct
  module type A = sig type t end
  module MyMap(X : A) = struct open X let f (x : t) = x end
end
