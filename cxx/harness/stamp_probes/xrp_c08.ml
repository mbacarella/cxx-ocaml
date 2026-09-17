module type MyT = sig type t val create : int -> t end
module MyMap(X : MyT) = struct type t = X.t let create x = X.create x end
module rec MyList : sig
  type t val create : int -> t val other : int
end = struct
  include MyMap(MyList)
  let other = 1
end
