/*
 * To change this license header, choose License Headers in Project Properties.
 * To change this template file, choose Tools | Templates
 * and open the template in the editor.
 */

layui.define(["layer"], function (exports) {
    var moduleName = 'drawer'
            , _layui = layui
            , $ = _layui.$
            , thisCss = []
            , u = undefined
            , l = false//状态
            , accuracy = 2;//精度
    var obj = {
        render: function (options) {
            var trigger = options.trigger
                    , position = (options.position == u ? "left" : options.position)
                    , wide = (options.specs == u ? "230" : options.specs)
                    , cache = wide
                    , wideNum = parseInt(wide)
                    , C = options.elem
                    , cid = C.replace(/#/g, "")
                    , open = options.open
                    , close = options.close;
            window.onresize = obj.findDimensions;
            var thisStyle = document.createElement('style');

            $(trigger).click(function () {
                $(C).show();
                //百分号转px像素
                if (cache.indexOf("%") != -1) {
                    if (position == "top" || position == "bottom") {
                        wide = obj.findDimensions()[1] * (wideNum / 100);
                    } else {
                        wide = obj.findDimensions()[0] * (wideNum / 100);
                    }
                }

                thisCss = [];
                if (position == "left" || position == "right") {
                    thisCss.push(C + '{z-index: 2005;background-color: #fff;border: 0;background-clip: padding-box;box-shadow: 0 4px 12px rgba(0,0,0,.1);overflow: hidden;transition: all 0.2s;height: 100%;position:absolute;top:0;bottom:0;width:' + wide + 'px;' + position + ':-' + wide + 'px;}');
                } else {
                    thisCss.push(C + '{z-index: 2005;background-color: #fff;border: 0;background-clip: padding-box;box-shadow: 0 4px 12px rgba(0,0,0,.1);overflow: hidden;transition: all 0.2s;width: 100%;position:absolute;height:' + wide + 'px;' + position + ':-' + wide + 'px;}');
                }
                thisStyle.innerHTML = thisCss.join('\n'), document.getElementsByTagName('head')[0].appendChild(thisStyle);
                switch (position) {
                    case 'left':
                        let left = $(C).offset().left;
                        if (left == 0) {
                            l = false;
                            $(C).offset({
                                left: '-' + wide
                            });
                            //if (left >= ('-' + wide))
                        } else {
                            l = true;
                            $(C).offset({
                                left: 0
                            });
                        }
                        break;
                    case 'right':
                        let right = obj.functiongGetRect(document.getElementById(cid)).right;
                        if ((right - obj.findDimensions()[0]) < accuracy) {
                            l = false;
                            $(C).css("right", "-" + wide + "px");
                        } else {
                            l = true;
                            $(C).css("right", 0);
                        }
                        break;
                    case 'top':
                        let top = $(C).offset().top;
                        if (top == 0) {
                            l = false;
                            $(C).offset({
                                top: '-' + wide
                            });
                            //if (top >= ('-' + wide))
                        } else {
                            l = true;
                            $(C).offset({
                                top: 0
                            });
                        }
                        break;
                    case 'bottom':
                        let bottom = obj.functiongGetRect(document.getElementById(cid)).bottom;
                        if ((bottom - obj.findDimensions()[1]) < accuracy) {
                            l = false;
                            $(C).css("bottom", "-" + wide + "px");
                        } else {
                            l = true;
                            $(C).css("bottom", 0);
                        }
                        break;
                    default:
                        layer.msg("position参数异常");
                        console.error("position参数异常");
                }
                obj.branchProup(C, position, wide, close);
                obj.type(l, open, close);
            });
        },

        /**
         *
         * @param {type} c
         * @param {type} position
         * @param {type} wide
         * @param {type} close
         * 遮罩
         */
        branchProup: function (c, position, wide, close) {
            if (document.getElementById("maskDiv")) {
                document.getElementById("maskDiv").style.display = 'block';
            } else {
                var mask = document.createElement('div');
                mask.id = 'maskDiv';
                mask.style.width = '100%';
                mask.style.height = '100%';
                mask.style.background = 'rgba(55,55,55,.6)';
                mask.style.position = 'fixed';
                mask.style.top = 0;
                mask.style.bottom = 0;
                mask.style.left = 0;
                mask.style.right = 0;
                mask.style.zIndex = 999;
                document.body.appendChild(mask);
            }
            //点击空白回收
            document.getElementById("maskDiv").onclick = function () {
                if (document.getElementById("maskDiv")) {
                    document.getElementById("maskDiv").style.display = 'none';
                    $(c).css(position, "-" + wide + "px");
                    if (close) {
                        close();
                    }
                }
            }
        },
        /**
         *
         * @param {type} l
         * @param {type} open
         * @param {type} close
         * 弹出、回收执行方法
         */
        type: function (l, open, close) {
            if (l) {
                if (open) {
                    open();
                }
            } else {
                document.getElementById("maskDiv").style.display = 'none';
                if (close) {
                    close();
                }
            }
        },
        /**
         * 获取相对于视窗的位置集合
         * @param {type} ele
         * @returns {layDrawerL#7.obj.functiongGetRect.layDrawerAnonym$5}
         */
        functiongGetRect: function (ele) {
            var rect = ele.getBoundingClientRect();
            var top = document.documentElement.clientTop;
            var left = document.documentElement.clientLeft;
            return{
                top: rect.top - top,
                bottom: rect.bottom - top,
                left: rect.left - left,
                right: rect.right - left
            }
        },
        /**
         * 有效宽高
         * @returns {Array}
         */
        findDimensions: function () {
            var winWidth = 0;
            var winHeight = 0;
            var wh = [];
            //获取窗口宽度
            if (window.innerWidth)
                winWidth = window.innerWidth;
            else if ((document.body) && (document.body.clientWidth))
                winWidth = document.body.clientWidth;
            //获取窗口高度
            if (window.innerHeight)
                winHeight = window.innerHeight;
            else if ((document.body) && (document.body.clientHeight))
                winHeight = document.body.clientHeight;
            if (document.documentElement && document.documentElement.clientHeight && document.documentElement.clientWidth) {
                winHeight = document.documentElement.clientHeight;
                winWidth = document.documentElement.clientWidth;
            }
            wh.push(winWidth);
            wh.push(winHeight);
            return wh;
        }
    };
    exports(moduleName, obj);
});