module type MyT = sig type 'a t = Succ of 'a t end
module MyMap(X : MyT) = X
module rec MyList : MyT = MyMap(MyList)
and MyList2 : MyT = MyMap(MyList2)
