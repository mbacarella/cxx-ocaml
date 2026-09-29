module type MyT = sig type t val create : int -> t end
module MyMap(X : MyT) = struct include X end
module rec MyList : MyT = MyMap(MyList)
and B : MyT = MyMap(MyList)
