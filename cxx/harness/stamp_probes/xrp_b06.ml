module type MyT = sig type t = { a : int } end
module MyMap(X : MyT) = struct include X end
module rec MyList : MyT = MyMap(MyList)
