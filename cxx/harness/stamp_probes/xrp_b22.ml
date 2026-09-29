module type MyT = sig type t val x : t end
module MyMap(X : MyT) = X
module rec MyList : MyT = MyMap(MyList)
