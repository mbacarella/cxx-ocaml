module P = struct
  module type MyT = sig type t end
  module MyMap(X : MyT) = X
  module N = MyMap(struct type t = int end)
end
