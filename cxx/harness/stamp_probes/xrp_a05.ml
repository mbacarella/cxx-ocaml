module type MyT = sig type t end
module P = struct
  module MyMap(X : MyT) = X
end
