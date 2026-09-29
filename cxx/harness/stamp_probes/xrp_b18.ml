module type MyT = sig type t val create : int -> t end
module MyMap(X : MyT) = X
module rec MyList : MyT = MyMap(MyList)
