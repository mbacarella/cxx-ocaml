module P = struct
module type MyT = sig type 'a t = Succ of 'a t end
module MyMap(X : MyT) = struct include X end
module rec MyList : MyT = MyMap(MyList)
end
