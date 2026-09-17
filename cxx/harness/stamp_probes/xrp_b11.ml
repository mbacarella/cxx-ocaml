module type MyT = sig type t val create : int -> t end
module MyMap(X : MyT) = struct include X end
module N = struct type t = int let create x = x end
module rec MyList : MyT = MyMap(N)
