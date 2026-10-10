// SPDX-License-Identifier: BSD-3-Clause
#include <QApplication>
#include <QPushButton>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <QWidget>
int main(int argc, char **argv) {
	QApplication app(argc, argv);
	QWidget window;
	window.setWindowTitle("Private Qt accessibility repro");
	auto *layout = new QVBoxLayout(&window);
	auto *tree   = new QTreeWidget;
	tree->setColumnCount(3);
	tree->setHeaderLabels({ "Name", "Ping", "Users" });
	auto *root = new QTreeWidgetItem(tree, { "Favorites" });
	root->setExpanded(true);
	root->setHidden(true);
	auto *button = new QPushButton("Add and select private test row");
	layout->addWidget(tree);
	layout->addWidget(button);
	QObject::connect(button, &QPushButton::clicked, &window, [=] {
		auto *child = new QTreeWidgetItem(root, { "Private fixture", "0", "0" });
		root->setHidden(false);
		tree->setCurrentItem(child);
		button->setFocus();
	});
	window.resize(600, 300);
	window.show();
	return app.exec();
}
