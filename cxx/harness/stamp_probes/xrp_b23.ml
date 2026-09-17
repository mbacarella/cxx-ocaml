module type MyT = sig type t end
module MyMap(X : MyT) = X
module rec MyList : MyT = MyMap(MyList)
