module P = struct
module type MyT = sig type 'a t = Succ of 'a t end
module MyMap(X : MyT) = X
module rec MyList : MyT = MyMap(MyList)
end
